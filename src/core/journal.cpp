#include "journal.hpp"
#include "../util/logging.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace tv {

UploadJournal::UploadJournal(std::string file_path, std::string file_hash, uint64_t file_size, int64_t meta_msg_id)
    : file_path_(std::move(file_path)),
      file_hash_(std::move(file_hash)),
      file_size_(file_size),
      meta_msg_id_(meta_msg_id) {}

std::string UploadJournal::journal_dir() {
    std::string base;
    if (auto* home = std::getenv("XDG_DATA_HOME")) {
        base = std::string(home) + "/televault";
    } else if (auto* home = std::getenv("HOME")) {
        base = std::string(home) + "/.local/share/televault";
    } else {
        base = "/tmp/televault";
    }
    auto dir = base + "/journal";
    std::filesystem::create_directories(dir);
    return dir;
}

std::optional<UploadJournal> UploadJournal::load(const std::string& file_hash) {
    if (file_hash.empty()) return std::nullopt;
    auto path = std::filesystem::path(journal_dir()) / (file_hash + ".json");
    if (!std::filesystem::exists(path)) return std::nullopt;

    try {
        std::ifstream f(path);
        if (!f) return std::nullopt;
        auto j = nlohmann::json::parse(f);

        UploadJournal journal;
        journal.file_path_ = j.value("file_path", "");
        journal.file_hash_ = j.value("file_hash", "");
        journal.file_size_ = j.value("file_size", static_cast<uint64_t>(0));
        journal.meta_msg_id_ = j.value("meta_msg_id", static_cast<int64_t>(0));
        if (j.contains("uploaded_chunks") && j["uploaded_chunks"].is_array()) {
            journal.uploaded_chunks_ = j["uploaded_chunks"].get<std::vector<ChunkInfo>>();
        }
        return journal;
    } catch (const std::exception& e) {
        spdlog::warn("Failed to read upload journal for {}: {}", file_hash, e.what());
        return std::nullopt;
    }
}

void UploadJournal::record_chunk(const ChunkInfo& chunk) {
    for (auto& c : uploaded_chunks_) {
        if (c.index == chunk.index) {
            c = chunk;
            return;
        }
    }
    uploaded_chunks_.push_back(chunk);
}

bool UploadJournal::save() const {
    if (file_hash_.empty()) return false;
    auto path = std::filesystem::path(journal_dir()) / (file_hash_ + ".json");

    try {
        nlohmann::json j;
        j["file_path"] = file_path_;
        j["file_hash"] = file_hash_;
        j["file_size"] = file_size_;
        j["meta_msg_id"] = meta_msg_id_;
        j["uploaded_chunks"] = uploaded_chunks_;

        std::ofstream f(path);
        if (!f) return false;
        f << j.dump(2);
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("Failed to write upload journal {}: {}", path.string(), e.what());
        return false;
    }
}

void UploadJournal::remove() const {
    if (file_hash_.empty()) return;
    auto path = std::filesystem::path(journal_dir()) / (file_hash_ + ".json");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

bool UploadJournal::has_chunk(int64_t chunk_idx) const {
    for (const auto& c : uploaded_chunks_) {
        if (c.index == chunk_idx && c.message_id != 0) return true;
    }
    return false;
}

std::optional<ChunkInfo> UploadJournal::get_chunk(int64_t chunk_idx) const {
    for (const auto& c : uploaded_chunks_) {
        if (c.index == chunk_idx && c.message_id != 0) return c;
    }
    return std::nullopt;
}

} // namespace tv
