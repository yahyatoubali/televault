#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <nlohmann/json.hpp>

namespace tv {

struct ChunkInfo {
    int64_t index{0};
    int64_t message_id{0};
    int32_t file_id{0};        // tdlib file ID for downloads
    uint64_t size{0};
    uint64_t offset{0};        // byte offset in the original file
    std::string hash;          // hash of the ciphertext / chunk
    std::string original_hash; // hash of the plaintext

    auto operator<=>(const ChunkInfo&) const = default;
};

inline void to_json(nlohmann::json& j, const ChunkInfo& c) {
    j = nlohmann::json{
        {"index", c.index},
        {"message_id", c.message_id},
        {"size", c.size},
        {"hash", c.hash}
    };
    if (!c.original_hash.empty()) {
        j["original_hash"] = c.original_hash;
    }
    if (c.file_id != 0) {
        j["file_id"] = c.file_id;
    }
    if (c.offset != 0) {
        j["offset"] = c.offset;
    }
}

inline void from_json(const nlohmann::json& j, ChunkInfo& c) {
    j.at("index").get_to(c.index);
    j.at("message_id").get_to(c.message_id);
    j.at("size").get_to(c.size);
    j.at("hash").get_to(c.hash);
    if (j.contains("original_hash") && !j["original_hash"].is_null()) {
        c.original_hash = j["original_hash"].get<std::string>();
    } else {
        c.original_hash.clear();
    }
    if (j.contains("file_id") && !j["file_id"].is_null()) {
        c.file_id = j["file_id"].get<int32_t>();
    } else {
        c.file_id = 0;
    }
    if (j.contains("offset") && !j["offset"].is_null()) {
        c.offset = j["offset"].get<uint64_t>();
    } else {
        c.offset = 0;
    }
}

namespace detail {

inline std::chrono::system_clock::time_point parse_timestamp(const nlohmann::json& j) {
    if (j.is_number_float()) {
        auto secs = j.get<double>();
        return std::chrono::system_clock::time_point(
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::duration<double>(secs)));
    } else if (j.is_number_integer()) {
        auto secs = j.get<int64_t>();
        return std::chrono::system_clock::from_time_t(static_cast<time_t>(secs));
    }
    return std::chrono::system_clock::now();
}

inline double timestamp_to_double(const std::chrono::system_clock::time_point& tp) {
    return std::chrono::duration<double>(tp.time_since_epoch()).count();
}

} // namespace detail

struct FileMetadata {
    std::string id;
    std::string name;
    uint64_t size{0};
    std::string hash;
    std::vector<ChunkInfo> chunks;
    bool encrypted{true};
    bool compressed{false};
    std::optional<double> compression_ratio;
    std::optional<std::string> mime_type;

    std::chrono::system_clock::time_point created_at{std::chrono::system_clock::now()};
    std::chrono::system_clock::time_point modified_at{created_at};
    std::chrono::system_clock::time_point updated_at{created_at}; // Alias for modified_at

    int64_t metadata_message_id{0}; // Telegram metadata message ID

    [[nodiscard]] size_t chunk_count() const noexcept { return chunks.size(); }
    [[nodiscard]] uint64_t total_stored_size() const noexcept {
        uint64_t total = 0;
        for (const auto& c : chunks) total += c.size;
        return total;
    }
    [[nodiscard]] bool is_complete() const {
        if (chunks.empty()) return false;
        std::vector<bool> seen(chunks.size(), false);
        for (const auto& c : chunks) {
            if (c.index < 0 || static_cast<size_t>(c.index) >= chunks.size()) return false;
            seen[static_cast<size_t>(c.index)] = true;
        }
        for (bool s : seen) {
            if (!s) return false;
        }
        return true;
    }
};

inline void to_json(nlohmann::json& j, const FileMetadata& m) {
    j = nlohmann::json{
        {"id", m.id},
        {"name", m.name},
        {"size", m.size},
        {"hash", m.hash},
        {"chunks", m.chunks},
        {"encrypted", m.encrypted},
        {"compressed", m.compressed},
        {"created_at", detail::timestamp_to_double(m.created_at)},
        {"modified_at", detail::timestamp_to_double(m.modified_at)},
        {"updated_at", detail::timestamp_to_double(m.modified_at)}
    };
    if (m.compression_ratio.has_value()) {
        j["compression_ratio"] = *m.compression_ratio;
    }
    if (m.mime_type.has_value()) {
        j["mime_type"] = *m.mime_type;
    }
    if (m.metadata_message_id != 0) {
        j["message_id"] = m.metadata_message_id;
    }
}

inline void from_json(const nlohmann::json& j, FileMetadata& m) {
    j.at("id").get_to(m.id);
    j.at("name").get_to(m.name);
    j.at("size").get_to(m.size);
    j.at("hash").get_to(m.hash);

    if (j.contains("chunks") && j["chunks"].is_array()) {
        m.chunks = j["chunks"].get<std::vector<ChunkInfo>>();
    } else {
        m.chunks.clear();
    }

    if (j.contains("encrypted") && !j["encrypted"].is_null()) {
        m.encrypted = j["encrypted"].get<bool>();
    } else {
        m.encrypted = true;
    }

    if (j.contains("compressed") && !j["compressed"].is_null()) {
        m.compressed = j["compressed"].get<bool>();
    } else {
        m.compressed = false;
    }

    if (j.contains("compression_ratio") && !j["compression_ratio"].is_null()) {
        m.compression_ratio = j["compression_ratio"].get<double>();
    } else {
        m.compression_ratio = std::nullopt;
    }

    if (j.contains("mime_type") && !j["mime_type"].is_null()) {
        m.mime_type = j["mime_type"].get<std::string>();
    } else {
        m.mime_type = std::nullopt;
    }

    if (j.contains("created_at") && !j["created_at"].is_null()) {
        m.created_at = detail::parse_timestamp(j["created_at"]);
    } else {
        m.created_at = std::chrono::system_clock::now();
    }

    if (j.contains("modified_at") && !j["modified_at"].is_null()) {
        m.modified_at = detail::parse_timestamp(j["modified_at"]);
        m.updated_at = m.modified_at;
    } else if (j.contains("updated_at") && !j["updated_at"].is_null()) {
        m.updated_at = detail::parse_timestamp(j["updated_at"]);
        m.modified_at = m.updated_at;
    } else {
        m.modified_at = m.created_at;
        m.updated_at = m.created_at;
    }

    if (j.contains("message_id") && !j["message_id"].is_null()) {
        m.metadata_message_id = j["message_id"].get<int64_t>();
    } else if (j.contains("metadata_message_id") && !j["metadata_message_id"].is_null()) {
        m.metadata_message_id = j["metadata_message_id"].get<int64_t>();
    } else {
        m.metadata_message_id = 0;
    }
}

} // namespace tv
