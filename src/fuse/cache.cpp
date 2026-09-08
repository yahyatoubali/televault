#include "cache.hpp"
#include <spdlog/spdlog.h>

namespace tv {

LRUChunkCache::LRUChunkCache(uint64_t max_bytes) : max_bytes_(max_bytes) {}

void LRUChunkCache::put(const std::string& file_id, int64_t chunk_index, std::vector<uint8_t> data) {
    std::unique_lock lock(mutex_);
    auto key = file_id + ":" + std::to_string(chunk_index);
    auto size = data.size();

    while (current_bytes_ + size > max_bytes_ && !entries_.empty()) {
        auto& old = entries_.back();
        current_bytes_ -= old.size;
        lookup_.erase(old.file_id + ":" + std::to_string(old.chunk_index));
        entries_.pop_back();
    }

    entries_.emplace_front(file_id, chunk_index, std::move(data), size);
    lookup_[key] = entries_.begin();
    current_bytes_ += size;
}

std::optional<std::vector<uint8_t>> LRUChunkCache::get(const std::string& file_id, int64_t chunk_index) {
    std::shared_lock lock(mutex_);
    auto key = file_id + ":" + std::to_string(chunk_index);
    if (auto it = lookup_.find(key); it != lookup_.end()) {
        return it->second->data;
    }
    return std::nullopt;
}

void LRUChunkCache::evict(const std::string& file_id) {
    std::unique_lock lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->file_id == file_id) {
            current_bytes_ -= it->size;
            lookup_.erase(it->file_id + ":" + std::to_string(it->chunk_index));
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

void LRUChunkCache::clear() {
    std::unique_lock lock(mutex_);
    entries_.clear();
    lookup_.clear();
    current_bytes_ = 0;
}

void LRUChunkCache::set_max_bytes(uint64_t max_bytes) {
    std::unique_lock lock(mutex_);
    max_bytes_ = max_bytes;
    while (current_bytes_ > max_bytes_ && !entries_.empty()) {
        auto& old = entries_.back();
        current_bytes_ -= old.size;
        lookup_.erase(old.file_id + ":" + std::to_string(old.chunk_index));
        entries_.pop_back();
    }
}

} // namespace tv
