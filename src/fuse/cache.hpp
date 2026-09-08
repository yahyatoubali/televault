#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <list>
#include <shared_mutex>
#include <optional>

namespace tv {

class LRUChunkCache {
public:
    explicit LRUChunkCache(uint64_t max_bytes);

    void put(const std::string& file_id, int64_t chunk_index, std::vector<uint8_t> data);
    std::optional<std::vector<uint8_t>> get(const std::string& file_id, int64_t chunk_index);
    void evict(const std::string& file_id);
    void clear();

private:
    struct CacheEntry {
        std::string file_id;
        int64_t chunk_index;
        std::vector<uint8_t> data;
        uint64_t size;
    };

    uint64_t max_bytes_;
    uint64_t current_bytes_{};
    std::list<CacheEntry> entries_;
    std::unordered_map<std::string, std::list<CacheEntry>::iterator> lookup_;
    mutable std::shared_mutex mutex_;
};

} // namespace tv
