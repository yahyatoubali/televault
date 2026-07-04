#pragma once

#include <vector>
#include <cstdint>
#include <span>
#include <string>
#include <functional>

namespace tv {

struct Chunk {
    int64_t index{};
    std::vector<uint8_t> data;
    uint64_t offset{};
    uint64_t original_size{};
    std::string hash;
};

using ChunkCallback = std::function<bool(Chunk)>;

std::vector<Chunk> iter_chunks(const std::string& file_path, uint64_t chunk_size);
void iter_chunks_async(const std::string& file_path, uint64_t chunk_size, ChunkCallback cb);

} // namespace tv
