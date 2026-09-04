#include "chunker.hpp"
#include "hash.hpp"
#include <fstream>
#include <vector>
#include <span>
#include <stdexcept>
#include <filesystem>
#include <cstdio>

namespace tv {

std::string Chunk::filename() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04ld.chunk", static_cast<long>(index));
    return std::string(buf);
}

uint64_t count_chunks(uint64_t file_size, uint64_t chunk_size) noexcept {
    if (file_size == 0 || chunk_size == 0) {
        return 0;
    }
    return (file_size + chunk_size - 1) / chunk_size;
}

ChunkStream::ChunkStream(std::string path, uint64_t chunk_size)
    : path_(std::move(path)), chunk_size_(chunk_size) {
    if (chunk_size_ == 0) {
        throw std::invalid_argument("chunk_size must be greater than 0");
    }
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) {
        throw std::runtime_error("Cannot open file: " + path_);
    }
    file_size_ = std::filesystem::file_size(path_, ec);
    if (ec) {
        throw std::runtime_error("Cannot get file size: " + path_);
    }
}

uint64_t ChunkStream::size() const noexcept {
    return count_chunks(file_size_, chunk_size_);
}

ChunkStream::Iterator ChunkStream::begin() const {
    if (file_size_ == 0) {
        return end();
    }
    return Iterator(path_, chunk_size_, file_size_, 0, 0);
}

ChunkStream::Iterator ChunkStream::end() const {
    return Iterator();
}

Chunk ChunkStream::operator[](size_t index) const {
    return read_chunk(path_, static_cast<int64_t>(index), chunk_size_);
}

std::vector<Chunk> ChunkStream::to_vector() const {
    std::vector<Chunk> result;
    result.reserve(static_cast<size_t>(size()));
    for (auto it = begin(); it != end(); ++it) {
        result.push_back(*it);
    }
    return result;
}

ChunkStream::Iterator::Iterator(const std::string& path, uint64_t chunk_size, uint64_t file_size, int64_t index, uint64_t offset)
    : path_(path), chunk_size_(chunk_size), file_size_(file_size), index_(index), offset_(offset), at_end_(false) {
    file_ = std::make_shared<std::ifstream>(path_, std::ios::binary);
    if (!file_->is_open()) {
        throw std::runtime_error("Cannot open file: " + path_);
    }
    fetch_next();
}

void ChunkStream::Iterator::fetch_next() {
    if (offset_ >= file_size_) {
        at_end_ = true;
        file_.reset();
        return;
    }

    auto to_read = std::min<uint64_t>(chunk_size_, file_size_ - offset_);
    std::vector<uint8_t> buf(to_read);

    file_->read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(to_read));
    auto read_bytes = file_->gcount();
    if (read_bytes <= 0) {
        at_end_ = true;
        file_.reset();
        return;
    }

    buf.resize(static_cast<size_t>(read_bytes));
    auto hash = hash_data(buf);

    current_.index = index_++;
    current_.data = std::move(buf);
    current_.offset = offset_;
    current_.original_size = static_cast<uint64_t>(read_bytes);
    current_.hash = std::move(hash);

    offset_ += static_cast<uint64_t>(read_bytes);
}

ChunkStream::Iterator& ChunkStream::Iterator::operator++() {
    fetch_next();
    return *this;
}

ChunkStream::Iterator ChunkStream::Iterator::operator++(int) {
    Iterator tmp = *this;
    fetch_next();
    return tmp;
}

bool ChunkStream::Iterator::operator==(const Iterator& other) const noexcept {
    if (at_end_ && other.at_end_) return true;
    if (at_end_ != other.at_end_) return false;
    return offset_ == other.offset_ && index_ == other.index_;
}

ChunkStream iter_chunks(const std::string& file_path, uint64_t chunk_size) {
    return ChunkStream(file_path, chunk_size);
}

void iter_chunks_async(const std::string& file_path, uint64_t chunk_size, ChunkCallback cb) {
    auto stream = iter_chunks(file_path, chunk_size);
    for (auto it = stream.begin(); it != stream.end(); ++it) {
        Chunk chunk = *it;
        if (!cb(std::move(chunk))) {
            break;
        }
    }
}

Chunk read_chunk(const std::string& file_path, int64_t index, uint64_t chunk_size) {
    if (chunk_size == 0) {
        throw std::invalid_argument("chunk_size must be positive");
    }
    std::error_code ec;
    auto file_size = std::filesystem::file_size(file_path, ec);
    if (ec) {
        throw std::runtime_error("Cannot get file size: " + file_path);
    }
    uint64_t offset = static_cast<uint64_t>(index) * chunk_size;
    if (offset >= file_size) {
        throw std::out_of_range("Chunk " + std::to_string(index) + " is out of range");
    }

    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + file_path);
    }

    file.seekg(static_cast<std::streamoff>(offset));
    auto to_read = std::min<uint64_t>(chunk_size, file_size - offset);
    std::vector<uint8_t> buf(to_read);
    file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(to_read));
    auto read_bytes = file.gcount();
    if (read_bytes <= 0) {
        throw std::runtime_error("Failed reading chunk " + std::to_string(index));
    }
    buf.resize(static_cast<size_t>(read_bytes));

    Chunk chunk;
    chunk.index = index;
    chunk.data = std::move(buf);
    chunk.offset = offset;
    chunk.original_size = static_cast<uint64_t>(read_bytes);
    chunk.hash = hash_data(chunk.data);
    return chunk;
}

} // namespace tv
