#pragma once

#include <string>
#include <cstdint>

namespace tv {

class ChunkWriter {
public:
    explicit ChunkWriter(const std::string& path, uint64_t expected_size);
    ~ChunkWriter();

    void write(int64_t chunk_index, uint64_t offset, const void* data, uint64_t size);
    void close();
    [[nodiscard]] bool is_complete() const;
    [[nodiscard]] uint64_t bytes_written() const { return bytes_written_; }

private:
    int fd_{-1};
    uint64_t expected_size_{};
    uint64_t bytes_written_{};
    std::string path_;
};

} // namespace tv
