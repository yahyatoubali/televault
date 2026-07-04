#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace tv {

struct OrphanInfo {
    int64_t message_id{};
    std::string type; // "file_chunk", "metadata", "snapshot"
    uint64_t size{};
};

struct GarbageCollectionResult {
    std::vector<OrphanInfo> orphans;
    std::vector<int64_t> partial_uploads;
    uint64_t reclaimable_bytes{};
};

GarbageCollectionResult collect_garbage(int64_t channel_id, bool dry_run = true);
std::vector<int64_t> cleanup_partial_uploads(int64_t channel_id);

} // namespace tv
