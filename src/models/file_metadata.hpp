#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <nlohmann/json.hpp>

namespace tv {

struct ChunkInfo {
    int64_t index{};
    int64_t message_id{};
    int32_t file_id{}; // tdlib file ID for downloads
    uint64_t size{};
    uint64_t offset{}; // byte offset in the original file
    std::string hash; // hash of the ciphertext
    std::string original_hash; // hash of the plaintext

    auto operator<=>(const ChunkInfo&) const = default;
};

struct FileMetadata {
    std::string id;
    std::string name;
    uint64_t size{};
    std::string hash;
    std::vector<ChunkInfo> chunks;
    bool encrypted{};
    bool compressed{};
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;

    int64_t metadata_message_id{};
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChunkInfo, index, message_id, file_id, size, offset, hash, original_hash)

inline void to_json(nlohmann::json& j, const FileMetadata& m) {
    j = nlohmann::json{
        {"id", m.id},
        {"name", m.name},
        {"size", m.size},
        {"hash", m.hash},
        {"chunks", m.chunks},
        {"encrypted", m.encrypted},
        {"compressed", m.compressed},
        {"created_at", std::chrono::duration_cast<std::chrono::seconds>(
                           m.created_at.time_since_epoch()).count()},
        {"updated_at", std::chrono::duration_cast<std::chrono::seconds>(
                           m.updated_at.time_since_epoch()).count()}
    };
}

inline void from_json(const nlohmann::json& j, FileMetadata& m) {
    j.at("id").get_to(m.id);
    j.at("name").get_to(m.name);
    j.at("size").get_to(m.size);
    j.at("hash").get_to(m.hash);
    j.at("chunks").get_to(m.chunks);
    j.at("encrypted").get_to(m.encrypted);
    j.at("compressed").get_to(m.compressed);
    int64_t ts{};
    j.at("created_at").get_to(ts);
    m.created_at = std::chrono::system_clock::from_time_t(ts);
    j.at("updated_at").get_to(ts);
    m.updated_at = std::chrono::system_clock::from_time_t(ts);
}

} // namespace tv
