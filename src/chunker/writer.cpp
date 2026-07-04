#include "writer.hpp"
#include <spdlog/spdlog.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace tv {

ChunkWriter::ChunkWriter(const std::string& path, uint64_t expected_size)
    : expected_size_(expected_size), path_(path) {
    fd_ = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd_ < 0) {
        spdlog::error("Failed to open output file: {}", path);
        return;
    }
    if (expected_size > 0) {
        ::fallocate(fd_, 0, 0, expected_size);
    }
}

ChunkWriter::~ChunkWriter() { close(); }

void ChunkWriter::write(int64_t, uint64_t offset, const void* data, uint64_t size) {
    if (fd_ < 0) return;
    ::pwrite(fd_, data, size, offset);
    bytes_written_ += size;
}

void ChunkWriter::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool ChunkWriter::is_complete() const {
    return bytes_written_ >= expected_size_;
}

} // namespace tv
