#include "vault.hpp"
#include "index.hpp"
#include "../telegram/client.hpp"
#include "../chunker/chunker.hpp"
#include "../chunker/hash.hpp"
#include "../compress/zstd.hpp"
#include "../crypto/aes256gcm.hpp"
#include "../crypto/kdf.hpp"
#include "../models/config.hpp"
#include "../util/format.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <array>
#include <chrono>
#include <span>
#include <algorithm>
#include <iostream>
#include <cctype>
#include <ranges>

namespace tv {

// ── Implementation ────────────────────────────────────────────────────
class TeleVault::Impl {
public:
    TelegramClient& tg;
    std::unique_ptr<IndexManager> index_mgr;
    int64_t channel_id_{};
    bool low_resource_{};

    // Encryption
    std::array<uint8_t, 32> master_key{};
    bool have_key{};

    explicit Impl(TelegramClient& tg_client) : tg(tg_client) {}

    bool initialize(int64_t channel_id, bool low_resource) {
        channel_id_ = channel_id;
        low_resource_ = low_resource;

        index_mgr = std::make_unique<IndexManager>(tg);
        return index_mgr->load(channel_id);
    }

    // ── Key derivation ──────────────────────────────────────────────
    void derive_master_key(const std::string& password) {
        std::string salt_str = "televault" + std::to_string(channel_id_);
        std::span<const uint8_t> salt(
            reinterpret_cast<const uint8_t*>(salt_str.data()), salt_str.size());
        auto key = derive_key(password, salt);
        master_key = key;
        have_key = true;
    }

    // ── Upload pipeline ─────────────────────────────────────────────
    bool push(const std::string& local_path, const VaultOptions& opts, ProgressCallback cb) {
        if (!std::filesystem::exists(local_path)) {
            spdlog::error("File does not exist: {}", local_path);
            return false;
        }

        uint64_t file_size = std::filesystem::file_size(local_path);
        std::string file_name = std::filesystem::path(local_path).filename().string();

        if (opts.encrypted) {
            if (opts.password.empty()) {
                spdlog::error("Password required for encryption");
                return false;
            }
            derive_master_key(opts.password);
        }

        if (cb) cb({0, file_size, "Hashing file...", 0});
        auto file_hash = hash_file(local_path);

        auto chunk_size = low_resource_
            ? static_cast<uint64_t>(32) * 1024 * 1024
            : static_cast<uint64_t>(256) * 1024 * 1024;

        if (cb) cb({0, file_size, "Chunking file...", 0});
        auto chunks = iter_chunks(local_path, chunk_size);

        // Process chunks: hash → compress → encrypt
        struct ProcChunk {
            int64_t index;
            uint64_t offset;
            uint64_t original_size;
            std::string original_hash;
            std::vector<uint8_t> data;
            std::string cipher_hash;
        };

        std::vector<ProcChunk> processed;
        processed.reserve(chunks.size());

        for (auto& chunk : chunks) {
            ProcChunk pc;
            pc.index = chunk.index;
            pc.offset = chunk.offset;
            pc.original_size = chunk.original_size;
            pc.original_hash = std::move(chunk.hash);

            auto data = std::move(chunk.data);

            if (opts.compressed && should_compress(file_name)) {
                data = compress_data(data);
            }
            if (opts.encrypted && have_key) {
                data = encrypt_chunk(data, master_key);
            }

            pc.data = std::move(data);
            pc.cipher_hash = hash_data(pc.data);
            processed.push_back(std::move(pc));

            if (cb) {
                cb({pc.offset + pc.original_size, file_size, "Processing...", 0});
            }
        }

        // Send metadata message
        FileMetadata meta;
        meta.id = file_hash.substr(0, 16);
        meta.name = file_name;
        meta.size = file_size;
        meta.hash = file_hash;
        meta.encrypted = opts.encrypted && have_key;
        meta.compressed = opts.compressed;
        meta.created_at = std::chrono::system_clock::now();
        meta.updated_at = meta.created_at;

        for (auto& pc : processed) {
            ChunkInfo ci;
            ci.index = pc.index;
            ci.offset = pc.offset;
            ci.size = pc.data.size();
            ci.hash = pc.cipher_hash;
            ci.original_hash = pc.original_hash;
            meta.chunks.push_back(std::move(ci));
        }

        if (cb) cb({0, file_size, "Sending metadata...", 0});
        nlohmann::json meta_json = meta;
        auto meta_msg_id = tg.send_text(channel_id_, meta_json.dump());
        if (meta_msg_id == 0) {
            spdlog::error("Failed to send metadata");
            return false;
        }
        meta.metadata_message_id = meta_msg_id;

        // Upload each chunk
        for (auto& pc : processed) {
            if (cb) {
                cb({static_cast<uint64_t>(pc.index + 1),
                    static_cast<uint64_t>(processed.size()),
                    std::format("Uploading chunk {}/{}...", pc.index + 1, processed.size()), 0});
            }

            auto tmp = std::filesystem::temp_directory_path() /
                std::format("tv_{}_{}", meta.id, pc.index);
            {
                std::ofstream f(tmp, std::ios::binary);
                f.write(reinterpret_cast<const char*>(pc.data.data()), pc.data.size());
            }

            auto send_result = tg.send_file_with_id(channel_id_, tmp.string(), meta_msg_id);
            std::filesystem::remove(tmp);

            if (send_result.message_id == 0) {
                spdlog::error("Failed to upload chunk {}", pc.index);
                return false;
            }

            meta.chunks[pc.index].message_id = send_result.message_id;
            meta.chunks[pc.index].file_id = send_result.file_id;
        }

        // Update metadata with message IDs
        nlohmann::json updated_json = meta;
        tg.edit_message(channel_id_, meta_msg_id, updated_json.dump());

        // Save index
        index_mgr->add_file(meta.id, meta_msg_id);
        if (!index_mgr->save(channel_id_)) {
            spdlog::error("Failed to save index");
            return false;
        }

        if (cb) cb({file_size, file_size, "Complete", 0});
        spdlog::info("Uploaded {} ({} chunks, {})", file_name, processed.size(), format_size(file_size));
        return true;
    }

    // ── Download pipeline ───────────────────────────────────────────
    bool pull(const std::string& vault_path, const std::string& output_path,
              const VaultOptions& opts, ProgressCallback cb)
    {
        // Find file
        auto meta = find_metadata(vault_path);
        if (!meta) {
            spdlog::error("File not found: {}", vault_path);
            return false;
        }

        if (meta->encrypted) {
            if (opts.password.empty()) {
                spdlog::error("Password required");
                return false;
            }
            derive_master_key(opts.password);
        }

        auto output = output_path.empty() ? vault_path : output_path;
        if (cb) cb({0, meta->size, "Downloading...", 0});

        // Pre-allocate output file
        {
            std::ofstream f(output, std::ios::binary);
            f.seekp(meta->size - 1);
            f.put(0);
        }

        uint64_t total = 0;
        for (auto& ci : meta->chunks) {
            if (cb) {
                cb({total, meta->size,
                    std::format("Chunk {}/{}...", ci.index + 1, meta->chunks.size()), 0});
            }

            // Download via message → extract file_id → download to local
            if (!tg.download_file_by_message(channel_id_, ci.message_id)) {
                spdlog::error("Failed to download chunk {}", ci.index);
                return false;
            }

            // Read downloaded file from tdlib's cache
            auto msg = tg.get_message(channel_id_, ci.message_id);
            auto finfo = tg.get_file_info(msg.file_id);
            if (!finfo || finfo->local_path.empty()) {
                spdlog::error("Cannot locate downloaded file for chunk {}", ci.index);
                return false;
            }

            std::ifstream f(finfo->local_path, std::ios::binary | std::ios::ate);
            auto fsize = f.tellg();
            f.seekg(0);
            std::vector<uint8_t> data(fsize);
            f.read(reinterpret_cast<char*>(data.data()), fsize);
            f.close();

            // Verify cipher hash
            if (hash_data(data) != ci.hash) {
                spdlog::error("Hash mismatch on chunk {}", ci.index);
                return false;
            }

            // Decrypt
            if (meta->encrypted && have_key) {
                data = decrypt_chunk(data, master_key);
            }

            // Decompress
            if (meta->compressed) {
                data = decompress_data(data);
            }

            // Verify original hash
            if (hash_data(data) != ci.original_hash) {
                spdlog::error("Original hash mismatch on chunk {}", ci.index);
                return false;
            }

            // Write at offset
            std::ofstream out(output, std::ios::binary | std::ios::in);
            out.seekp(ci.offset);
            out.write(reinterpret_cast<const char*>(data.data()), data.size());
            total += data.size();

            if (cb) {
                cb({total, meta->size,
                    std::format("Downloaded chunk {}/{}", ci.index + 1, meta->chunks.size()),
                    static_cast<double>(total) / std::chrono::duration<double>(
                        std::chrono::steady_clock::now().time_since_epoch()).count()});
            }
        }

        // Verify file hash
        if (hash_file(output) != meta->hash) {
            spdlog::error("File hash mismatch — download may be corrupted");
            return false;
        }

        if (cb) cb({meta->size, meta->size, "Complete", 0});
        spdlog::info("Downloaded {} (verified)", output);
        return true;
    }

    bool cat(const std::string& vault_path, ProgressCallback cb) {
        auto meta = find_metadata(vault_path);
        if (!meta) return false;

        for (auto& ci : meta->chunks) {
            if (!tg.download_file_by_message(channel_id_, ci.message_id)) break;

            auto msg = tg.get_message(channel_id_, ci.message_id);
            auto finfo = tg.get_file_info(msg.file_id);
            if (!finfo || finfo->local_path.empty()) break;

            std::ifstream f(finfo->local_path, std::ios::binary);
            std::vector<char> buf(65536);
            while (f.read(buf.data(), buf.size())) {
                std::cout.write(buf.data(), f.gcount());
            }
            if (f.gcount() > 0) std::cout.write(buf.data(), f.gcount());
        }
        return true;
    }

    // ── Filesystem operations ───────────────────────────────────────
    std::vector<FileEntry> list_files() const {
        std::vector<FileEntry> entries;
        for (auto& [fid, mid] : index_mgr->index().files) {
            auto meta = get_metadata(mid);
            if (!meta) continue;
            entries.push_back(make_entry(*meta));
        }
        return entries;
    }

    std::vector<FileEntry> find_files(const std::string& query) const {
        std::vector<FileEntry> results;
        auto lq = query;
        std::ranges::transform(lq, lq.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });

        for (auto& [fid, mid] : index_mgr->index().files) {
            auto meta = get_metadata(mid);
            if (!meta) continue;
            auto ln = meta->name;
            std::ranges::transform(ln, ln.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
            if (ln.find(lq) != std::string::npos) {
                results.push_back(make_entry(*meta));
            }
        }
        return results;
    }

    std::optional<FileMetadata> get_file_info(const std::string& path) const {
        return find_metadata(path);
    }

    bool delete_file(const std::string& path) {
        for (auto& [fid, mid] : index_mgr->index().files) {
            auto meta = get_metadata(mid);
            if (!meta || meta->name != path) continue;

            std::vector<int64_t> ids{mid};
            for (auto& ci : meta->chunks) ids.push_back(ci.message_id);
            tg.delete_messages(channel_id_, ids);
            index_mgr->remove_file(fid);
            index_mgr->save(channel_id_);
            return true;
        }
        return false;
    }

    bool verify_file(const std::string& path) {
        auto meta = find_metadata(path);
        if (!meta) return false;

        for (auto& ci : meta->chunks) {
            auto msg = tg.get_message(channel_id_, ci.message_id);
            if (msg.id == 0) {
                spdlog::error("Chunk {} message not found", ci.index);
                return false;
            }
        }
        return true;
    }

private:
    // ── Helpers ─────────────────────────────────────────────────────
    std::optional<FileMetadata> get_metadata(int64_t msg_id) const {
        auto msg = tg.get_message(channel_id_, msg_id);
        if (msg.text.empty()) return std::nullopt;
        try {
            return nlohmann::json::parse(msg.text).get<FileMetadata>();
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<FileMetadata> find_metadata(const std::string& path) const {
        for (auto& [fid, mid] : index_mgr->index().files) {
            auto meta = get_metadata(mid);
            if (meta && meta->name == path) return meta;
        }
        return std::nullopt;
    }

    static FileEntry make_entry(const FileMetadata& meta) {
        FileEntry e;
        e.id = meta.id;
        e.name = meta.name;
        e.size = meta.size;
        e.hash = meta.hash;
        e.encrypted = meta.encrypted;
        e.compressed = meta.compressed;
        e.chunk_count = static_cast<int>(meta.chunks.size());
        e.created_at = meta.created_at;
        return e;
    }
};

// ── Public API ────────────────────────────────────────────────────────
TeleVault::TeleVault(TelegramClient& tg) : impl_(std::make_unique<Impl>(tg)) {}
TeleVault::~TeleVault() = default;

bool TeleVault::initialize(int64_t channel_id, bool low_resource) {
    return impl_->initialize(channel_id, low_resource);
}

bool TeleVault::push(const std::string& path, const VaultOptions& opts, ProgressCallback cb) {
    return impl_->push(path, opts, std::move(cb));
}

bool TeleVault::pull(const std::string& path, const std::string& output,
                     const VaultOptions& opts, ProgressCallback cb) {
    return impl_->pull(path, output, opts, std::move(cb));
}

bool TeleVault::cat(const std::string& path, ProgressCallback cb) {
    return impl_->cat(path, std::move(cb));
}

std::vector<FileEntry> TeleVault::list_files() const {
    return impl_->list_files();
}

std::vector<FileEntry> TeleVault::find_files(const std::string& query) const {
    return impl_->find_files(query);
}

std::optional<FileMetadata> TeleVault::get_file_info(const std::string& path) const {
    return impl_->get_file_info(path);
}

bool TeleVault::delete_file(const std::string& path) {
    return impl_->delete_file(path);
}

bool TeleVault::verify_file(const std::string& path) {
    return impl_->verify_file(path);
}

} // namespace tv
