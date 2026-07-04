#include "chunker.hpp"
#include "hash.hpp"
#include <fstream>
#include <vector>
#include <span>
#include <stdexcept>
#include <filesystem>

namespace tv {

std::vector<Chunk> iter_chunks(const std::string& file_path, uint64_t chunk_size) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + file_path);
    }

    auto file_size = std::filesystem::file_size(file_path);
    std::vector<Chunk> chunks;
    int64_t index = 0;
    uint64_t offset = 0;

    while (offset < file_size) {
        auto to_read = std::min<uint64_t>(chunk_size, file_size - offset);
        std::vector<uint8_t> buf(to_read);

        file.read(reinterpret_cast<char*>(buf.data()), to_read);
        auto read = file.gcount();
        if (read <= 0) break;

        buf.resize(read);
        auto hash = hash_data(buf);

        Chunk chunk;
        chunk.index = index++;
        chunk.data = std::move(buf);
        chunk.offset = offset;
        chunk.original_size = read;
        chunk.hash = std::move(hash);

        chunks.push_back(std::move(chunk));
        offset += read;
    }

    return chunks;
}

void iter_chunks_async(const std::string& file_path, uint64_t chunk_size, ChunkCallback cb) {
    auto chunks = iter_chunks(file_path, chunk_size);
    for (auto& chunk : chunks) {
        if (!cb(std::move(chunk))) break;
    }
}

} // namespace tv
