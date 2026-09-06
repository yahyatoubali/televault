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
#include "../compress/zstd.hpp"
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
        tg.set_channel(channel_id);

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

    // ── Upload pipeline (Streaming chunking with bounded RAM) ──────
    bool push(const std::string& local_path, const VaultOptions& opts, ProgressCallback cb) {
        if (!std::filesystem::exists(local_path)) {
            spdlog::error("File does not exist: {}", local_path);
            return false;
        }

        uint64_t file_size = std::filesystem::file_size(local_path);
        std::string file_name = std::filesystem::path(local_path).filename().string();

        if (opts.encrypted && opts.password.empty()) {
            spdlog::error("Password required for encryption");
            return false;
        }

        if (cb) cb({0, file_size, "Hashing file...", 0});
        auto file_hash = hash_file(local_path);

        auto chunk_size = low_resource_
            ? static_cast<uint64_t>(32) * 1024 * 1024
            : static_cast<uint64_t>(256) * 1024 * 1024;

        uint64_t total_chunks = file_size == 0 ? 1 : ((file_size + chunk_size - 1) / chunk_size);

        // Send initial thread-root metadata message
        FileMetadata meta;
        meta.id = file_hash.substr(0, 16);
        meta.name = file_name;
        meta.size = file_size;
        meta.hash = file_hash;
        meta.encrypted = opts.encrypted;
        meta.compressed = opts.compressed;
        meta.created_at = std::chrono::system_clock::now();
        meta.updated_at = meta.created_at;

        if (cb) cb({0, file_size, "Initializing metadata...", 0});
        nlohmann::json meta_json = meta;
        auto meta_msg_id = tg.send_text(channel_id_, meta_json.dump());
        if (meta_msg_id == 0) {
            spdlog::error("Failed to send metadata root message");
            return false;
        }
        meta.metadata_message_id = meta_msg_id;

        std::ifstream file(local_path, std::ios::binary);
        if (!file && file_size > 0) {
            spdlog::error("Failed to open file for reading: {}", local_path);
            return false;
        }

        uint64_t bytes_processed = 0;
        meta.chunks.reserve(total_chunks);

        for (uint64_t chunk_idx = 0; chunk_idx < total_chunks; ++chunk_idx) {
            uint64_t current_offset = chunk_idx * chunk_size;
            uint64_t this_chunk_size = (file_size > current_offset)
                ? std::min(chunk_size, file_size - current_offset)
                : 0;

            std::vector<uint8_t> chunk_data(this_chunk_size);
            if (this_chunk_size > 0) {
                file.seekg(current_offset);
                file.read(reinterpret_cast<char*>(chunk_data.data()), this_chunk_size);
                if (!file && !file.eof()) {
                    spdlog::error("Failed to read chunk {} from file", chunk_idx);
                    return false;
                }
            }

            ChunkInfo ci;
            ci.index = static_cast<int64_t>(chunk_idx);
            ci.offset = current_offset;
            ci.original_hash = hash_data(chunk_data);

            if (opts.compressed && should_compress(file_name)) {
                chunk_data = compress_data(chunk_data);
            }
            if (opts.encrypted) {
                chunk_data = encrypt_chunk(chunk_data, opts.password);
            }

            ci.size = chunk_data.size();
            ci.hash = hash_data(chunk_data);

            // Write chunk to temp file for TDLib upload
            auto tmp = std::filesystem::temp_directory_path() /
                std::format("tv_{}_{}_{}", meta.id, chunk_idx, std::rand());
            {
                std::ofstream f(tmp, std::ios::binary);
                if (!f.write(reinterpret_cast<const char*>(chunk_data.data()), chunk_data.size())) {
                    spdlog::error("Failed to write temp chunk file: {}", tmp.string());
                    std::filesystem::remove(tmp);
                    return false;
                }
            }

            // Immediately free chunk data from RAM
            chunk_data.clear();
            chunk_data.shrink_to_fit();

            if (cb) {
                cb({bytes_processed, file_size,
                    std::format("Uploading chunk {}/{}...", chunk_idx + 1, total_chunks), 0});
            }

            auto send_result = tg.send_file_with_id(channel_id_, tmp.string(), meta_msg_id);
            std::filesystem::remove(tmp);

            if (send_result.message_id == 0) {
                spdlog::error("Failed to upload chunk {}", chunk_idx);
                return false;
            }

            ci.message_id = send_result.message_id;
            ci.file_id = send_result.file_id;
            meta.chunks.push_back(std::move(ci));

            bytes_processed += this_chunk_size;
            if (cb) {
                cb({bytes_processed, file_size,
                    std::format("Uploaded chunk {}/{}", chunk_idx + 1, total_chunks), 0});
            }
        }

        // Update metadata message with complete chunk list
        nlohmann::json updated_json = meta;
        if (!tg.edit_message(channel_id_, meta_msg_id, updated_json.dump())) {
            spdlog::error("Failed to update metadata message with chunk list");
            return false;
        }

        // Save index
        index_mgr->add_file(meta.id, meta_msg_id);
        if (!index_mgr->save(channel_id_)) {
            spdlog::error("Failed to save index");
            return false;
        }

        if (cb) cb({file_size, file_size, "Complete", 0});
        spdlog::info("Uploaded {} ({} chunks, {})", file_name, meta.chunks.size(), format_size(file_size));
        return true;
    }

    // ── Download pipeline (Atomic write with temporary swap) ────────
    bool pull(const std::string& vault_path, const std::string& output_path,
              const VaultOptions& opts, ProgressCallback cb)
    {
        auto meta = find_metadata(vault_path);
        if (!meta) {
            spdlog::error("File not found: {}", vault_path);
            return false;
        }

        if (meta->encrypted && opts.password.empty()) {
            spdlog::error("Password required for decryption");
            return false;
        }

        auto output = output_path.empty() ? vault_path : output_path;
        auto temp_output = output + ".tvt_tmp." + std::to_string(std::rand());

        if (cb) cb({0, meta->size, "Downloading...", 0});

        // Pre-allocate temporary output file
        if (meta->size > 0) {
            std::ofstream f(temp_output, std::ios::binary);
            f.seekp(meta->size - 1);
            f.put(0);
            if (!f) {
                spdlog::error("Failed to pre-allocate output file: {}", temp_output);
                std::filesystem::remove(temp_output);
                return false;
            }
        }

        std::string salt_str = "televault" + std::to_string(channel_id_);
        std::span<const uint8_t> fallback_salt(
            reinterpret_cast<const uint8_t*>(salt_str.data()), salt_str.size());

        uint64_t total = 0;
        for (auto& ci : meta->chunks) {
            if (cb) {
                cb({total, meta->size,
                    std::format("Chunk {}/{}...", ci.index + 1, meta->chunks.size()), 0});
            }

            if (!tg.download_file_by_message(channel_id_, ci.message_id)) {
                spdlog::error("Failed to download chunk {}", ci.index);
                std::filesystem::remove(temp_output);
                return false;
            }

            auto msg = tg.get_message(channel_id_, ci.message_id);
            auto finfo = tg.get_file_info(msg.file_id);
            if (!finfo || finfo->local_path.empty()) {
                spdlog::error("Cannot locate downloaded file for chunk {}", ci.index);
                std::filesystem::remove(temp_output);
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
                spdlog::error("Cipher hash mismatch on chunk {}", ci.index);
                std::filesystem::remove(temp_output);
                return false;
            }

            // Decrypt
            if (meta->encrypted) {
                try {
                    data = decrypt_chunk(data, opts.password, fallback_salt);
                } catch (const std::exception& e) {
                    spdlog::error("Decryption failed on chunk {}: {}", ci.index, e.what());
                    std::filesystem::remove(temp_output);
                    return false;
                }
            }

            // Decompress
            if (meta->compressed) {
                data = decompress_data(data);
            }

            // Verify original hash
            if (!ci.original_hash.empty() && hash_data(data) != ci.original_hash) {
                spdlog::error("Original hash mismatch on chunk {}", ci.index);
                std::filesystem::remove(temp_output);
                return false;
            }

            // Write chunk at offset in temporary file
            {
                std::ofstream out(temp_output, std::ios::binary | std::ios::in | std::ios::out);
                out.seekp(ci.offset);
                out.write(reinterpret_cast<const char*>(data.data()), data.size());
            }
            total += data.size();

            if (cb) {
                cb({total, meta->size,
                    std::format("Downloaded chunk {}/{}", ci.index + 1, meta->chunks.size()),
                    static_cast<double>(total) / std::chrono::duration<double>(
                        std::chrono::steady_clock::now().time_since_epoch()).count()});
            }
        }

        // Verify full file hash before finalizing
        if (meta->size > 0) {
            auto final_hash = hash_file(temp_output);
            if (final_hash != meta->hash) {
                spdlog::error("File hash mismatch — download may be corrupted: computed {} != expected {}",
                              final_hash, meta->hash);
                std::filesystem::remove(temp_output);
                return false;
            }
        }

        // Atomically replace target destination
        std::error_code ec;
        std::filesystem::rename(temp_output, output, ec);
        if (ec) {
            std::filesystem::copy_file(temp_output, output, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(temp_output);
            if (ec) {
                spdlog::error("Failed to commit downloaded file: {}", ec.message());
                return false;
            }
        }

        if (cb) cb({meta->size, meta->size, "Complete", 0});
        spdlog::info("Downloaded {} (verified)", output);
        return true;
    }

    bool cat(const std::string& vault_path, const VaultOptions& opts, ProgressCallback cb) {
        auto meta = find_metadata(vault_path);
        if (!meta) return false;

        if (meta->encrypted && opts.password.empty()) {
            spdlog::error("File is encrypted — password required for cat");
            return false;
        }

        std::string salt_str = "televault" + std::to_string(channel_id_);
        std::span<const uint8_t> fallback_salt(
            reinterpret_cast<const uint8_t*>(salt_str.data()), salt_str.size());

        for (auto& ci : meta->chunks) {
            if (!tg.download_file_by_message(channel_id_, ci.message_id)) break;

            auto msg = tg.get_message(channel_id_, ci.message_id);
            auto finfo = tg.get_file_info(msg.file_id);
            if (!finfo || finfo->local_path.empty()) break;

            std::ifstream f(finfo->local_path, std::ios::binary);
            if (!f) break;

            std::vector<uint8_t> file_data((std::istreambuf_iterator<char>(f)),
                                            std::istreambuf_iterator<char>());

            auto data = std::move(file_data);
            if (meta->encrypted) {
                try {
                    data = decrypt_chunk(data, opts.password, fallback_salt);
                } catch (const std::exception& e) {
                    spdlog::error("Decryption failed for chunk {}: {}", ci.index, e.what());
                    return false;
                }
            }

            if (meta->compressed) {
                data = decompress_data(data);
            }
            std::cout.write(reinterpret_cast<const char*>(data.data()), data.size());
            if (!std::cout) {
                spdlog::error("Write to stdout failed");
                return false;
            }
        }
        return true;
    }

    // ── Disaster Recovery (Rebuild index from channel messages) ──────
    bool recover_index() {
        spdlog::info("Starting index recovery from channel history...");
        auto history = tg.get_chat_history(channel_id_, 0, 100);
        int recovered = 0;
        for (const auto& msg : history) {
            if (msg.text.empty()) continue;
            try {
                auto j = nlohmann::json::parse(msg.text);
                if (j.contains("id") && j.contains("chunks") && j.contains("name")) {
                    auto meta = j.get<FileMetadata>();
                    index_mgr->add_file(meta.id, msg.id);
                    recovered++;
                    spdlog::info("Recovered entry: fid={} name={} mid={}", meta.id, meta.name, msg.id);
                }
            } catch (...) {
                // Not a valid FileMetadata JSON message, ignore
            }
        }
        if (recovered > 0) {
            index_mgr->save(channel_id_);
            spdlog::info("Recovery complete: {} files found and index updated", recovered);
            return true;
        }
        spdlog::warn("No file metadata found to recover");
        return false;
    }

    // ── Filesystem operations ───────────────────────────────────────
    std::vector<FileEntry> list_files() const {
        std::vector<FileEntry> entries;
        bool index_updated = false;
        spdlog::debug("list_files: index contains {} files", index_mgr->index().files.size());
        for (auto& [fid, mid] : index_mgr->index().files) {
            spdlog::debug("list_files: entry fid={} mid={}", fid, mid);
            auto meta = get_metadata(mid);
            if (!meta) {
                spdlog::warn("list_files: get_metadata failed for fid={} mid={}", fid, mid);
                continue;
            }
            if (meta->metadata_message_id != 0 && meta->metadata_message_id != mid) {
                const_cast<IndexManager&>(*index_mgr).add_file(fid, meta->metadata_message_id);
                index_updated = true;
            }
            entries.push_back(make_entry(*meta));
        }
        if (index_updated) {
            const_cast<IndexManager&>(*index_mgr).save(channel_id_);
            spdlog::info("list_files: auto-healed index saved to Telegram");
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
            if (ln.find(lq) != std::string::npos || meta->id.find(lq) != std::string::npos) {
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
            if (!meta) continue;
            if (meta->name != path && meta->id != path && fid != path &&
                !(path.size() >= 6 && meta->id.starts_with(path))) {
                continue;
            }

            int64_t target_mid = meta->metadata_message_id != 0 ? meta->metadata_message_id : mid;
            std::vector<int64_t> ids{target_mid};
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
        auto try_parse = [this](int64_t id) -> std::optional<FileMetadata> {
            auto msg = tg.get_message(channel_id_, id);
            if (msg.text.empty()) return std::nullopt;
            try {
                auto j = nlohmann::json::parse(msg.text);
                auto meta = j.get<FileMetadata>();
                meta.metadata_message_id = id;
                return meta;
            } catch (const std::exception& e) {
                return std::nullopt;
            }
        };

        auto meta = try_parse(msg_id);
        if (meta) return meta;

        // Self-healing: if msg_id was off (e.g. pinned notification or temp id offset), check nearby IDs
        int64_t seq = msg_id >> 20;
        for (int64_t offset : {1, -1, 2, -2, 3, -3}) {
            int64_t adj_id = (seq + offset) << 20;
            auto adj_meta = try_parse(adj_id);
            if (adj_meta) {
                spdlog::info("get_metadata: self-healed msg_id {} -> {}", msg_id, adj_id);
                return adj_meta;
            }
        }
        return std::nullopt;
    }

    std::optional<FileMetadata> find_metadata(const std::string& path) const {
        for (auto& [fid, mid] : index_mgr->index().files) {
            auto meta = get_metadata(mid);
            if (meta) {
                if (meta->name == path || meta->id == path || fid == path ||
                    (path.size() >= 6 && meta->id.starts_with(path))) {
                    return meta;
                }
            }
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

bool TeleVault::cat(const std::string& path, const VaultOptions& opts, ProgressCallback cb) {
    return impl_->cat(path, opts, std::move(cb));
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

bool TeleVault::recover_index() {
    return impl_->recover_index();
}

} // namespace tv
