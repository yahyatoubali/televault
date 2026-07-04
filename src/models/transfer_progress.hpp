#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct ChunkProgress {
    int64_t index{};
    int64_t message_id{};
    uint64_t bytes_transferred{};
    bool completed{};
    uint32_t crc32{};
};

struct TransferProgress {
    std::string file_id;
    std::string file_name;
    uint64_t file_size{};
    std::vector<ChunkProgress> chunks;
    std::string mode; // "upload" or "download"
    std::string state; // "in_progress", "completed", "failed"
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChunkProgress, index, message_id, bytes_transferred, completed, crc32)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TransferProgress, file_id, file_name, file_size, chunks, mode, state)
