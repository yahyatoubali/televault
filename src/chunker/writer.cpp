#include "writer.hpp"
#include "chunker.hpp"
#include <spdlog/spdlog.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <filesystem>
#include <stdexcept>
#include <cstring>
#include <cerrno>

namespace tv {

ChunkWriter::ChunkWriter(const std::string& path, uint64_t expected_size, uint64_t chunk_size)
    : expected_size_(expected_size), chunk_size_(chunk_size), path_(path) {
    // Automatically create parent directories
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) {
            spdlog::error("Failed to create parent directories for {}: {}", path, ec.message());
            throw std::runtime_error("Failed to create parent directories for: " + path);
        }
    }

    fd_ = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd_ < 0) {
        spdlog::error("Failed to open output file {}: {}", path, std::strerror(errno));
        throw std::runtime_error("Failed to open output file: " + path + " (" + std::strerror(errno) + ")");
    }

    // Pre-allocate file if non-zero size
    if (expected_size > 0) {
#if defined(__APPLE__) && defined(__MACH__)
        fstore_t fst = {F_ALLOCATECONTIG, F_PEOFPOSMODE, 0, static_cast<off_t>(expected_size), 0};
        if (::fcntl(fd_, F_PREALLOCATE, &fst) == -1) {
            fst.fst_flags = F_ALLOCATEALL;
            ::fcntl(fd_, F_PREALLOCATE, &fst);
        }
        ::ftruncate(fd_, static_cast<off_t>(expected_size));
#elif defined(__linux__)
        if (::fallocate(fd_, 0, 0, static_cast<off_t>(expected_size)) != 0 && errno != EOPNOTSUPP && errno != ENOSYS) {
            spdlog::warn("fallocate failed for {} ({}), continuing without pre-allocation", path, std::strerror(errno));
        }
#else
        ::ftruncate(fd_, static_cast<off_t>(expected_size));
#endif
    }
}

ChunkWriter::~ChunkWriter() {
    close();
}

void ChunkWriter::write(int64_t chunk_index, uint64_t offset, const void* data, uint64_t size) {
    if (data == nullptr && size > 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) {
        return;
    }

    // Idempotent duplicate check
    if (chunk_index >= 0 && written_chunks_.contains(chunk_index)) {
        spdlog::debug("Chunk {} already written to {}, skipping duplicate", chunk_index, path_);
        return;
    }

    if (size > 0) {
        auto written = ::pwrite(fd_, data, size, static_cast<off_t>(offset));
        if (written < 0 || static_cast<uint64_t>(written) != size) {
            spdlog::error("Short write to output file {}: wrote {} of {} bytes ({})",
                          path_, written, size, std::strerror(errno));
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("Write error on file: " + path_);
        }
    }

    if (chunk_index >= 0) {
        written_chunks_.insert(chunk_index);
    }
    bytes_written_ += size;
}

void ChunkWriter::write(int64_t chunk_index, uint64_t offset, std::span<const uint8_t> data) {
    write(chunk_index, offset, data.data(), data.size());
}

void ChunkWriter::write_chunk(const Chunk& chunk) {
    uint64_t offset = chunk.offset;
    if (offset == 0 && chunk.index > 0 && chunk_size_ > 0) {
        offset = static_cast<uint64_t>(chunk.index) * chunk_size_;
    }
    write(chunk.index, offset, chunk.data.data(), chunk.data.size());
}

void ChunkWriter::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool ChunkWriter::is_complete() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (expected_size_ == 0) {
        return true;
    }
    return bytes_written_ >= expected_size_;
}

bool ChunkWriter::is_complete(size_t expected_chunks) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return written_chunks_.size() == expected_chunks;
}

std::vector<int64_t> ChunkWriter::missing_chunks(size_t expected_chunks) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int64_t> missing;
    for (size_t i = 0; i < expected_chunks; ++i) {
        if (!written_chunks_.contains(static_cast<int64_t>(i))) {
            missing.push_back(static_cast<int64_t>(i));
        }
    }
    return missing;
}

uint64_t ChunkWriter::bytes_written() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_written_;
}

const std::unordered_set<int64_t>& ChunkWriter::written_chunks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return written_chunks_;
}

} // namespace tv
