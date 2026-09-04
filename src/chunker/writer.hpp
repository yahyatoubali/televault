#pragma once

#include <string>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>
#include <mutex>

namespace tv {

struct Chunk;

class ChunkWriter {
public:
    explicit ChunkWriter(const std::string& path, uint64_t expected_size, uint64_t chunk_size = 0);
    ~ChunkWriter();

    void write(int64_t chunk_index, uint64_t offset, const void* data, uint64_t size);
    void write(int64_t chunk_index, uint64_t offset, std::span<const uint8_t> data);
    void write_chunk(const Chunk& chunk);
    void close();

    [[nodiscard]] bool is_complete() const;
    [[nodiscard]] bool is_complete(size_t expected_chunks) const;
    [[nodiscard]] std::vector<int64_t> missing_chunks(size_t expected_chunks) const;
    [[nodiscard]] uint64_t bytes_written() const;
    [[nodiscard]] const std::unordered_set<int64_t>& written_chunks() const;

private:
    int fd_{-1};
    uint64_t expected_size_{0};
    uint64_t chunk_size_{0};
    uint64_t bytes_written_{0};
    std::string path_;
    std::unordered_set<int64_t> written_chunks_;
    mutable std::mutex mutex_;
};

} // namespace tv
