#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_set>

namespace tv {

class TelegramClient;
class TeleVault;

struct OrphanInfo {
    int64_t message_id{};
    std::string type; // "file_chunk", "metadata", "snapshot"
    uint64_t size{};
};

struct GarbageCollectionResult {
    std::vector<OrphanInfo> orphans;
    std::vector<std::string> partial_uploads; // index file_ids with unfetchable metadata
    uint64_t reclaimable_bytes{};
    uint64_t scanned_messages{};
    size_t unresolved_index_entries{}; // indexed metas that failed to load
};

// Normalizes app-level and raw TDLib message IDs to the canonical
// (server_id << 20) form so index/history IDs compare equal.
[[nodiscard]] inline int64_t normalize_message_id(int64_t id) noexcept {
    constexpr int64_t kSeqBits = 1LL << 20;
    if (id < kSeqBits) return id << 20;
    return (id >> 20) << 20;
}

// Pure scan over one channel history batch: every id not in `referenced`
// (canonical form) is reported; pinned index is always referenced by the
// caller. Text that parses as TeleVault metadata/snapshot JSON is typed
// accordingly, other text is left alone (never auto-deleted), and
// text-less messages are treated as chunk documents.
[[nodiscard]] std::vector<OrphanInfo> find_orphans(
    const std::vector<std::pair<int64_t, std::string>>& history,
    const std::unordered_set<int64_t>& referenced);

// Full scan: builds the referenced set from the pinned index, every
// indexed file (live + trashed) and the snapshot index, pages channel
// history, classifies leftovers, and deletes them unless dry_run.
GarbageCollectionResult collect_garbage(TelegramClient& tg, TeleVault& vault,
                                        int64_t channel_id, bool dry_run = true);

// Index entries whose metadata message can no longer be fetched
// (interrupted pushes / manually deleted messages). With dry_run=false
// they are dropped from the local index (channel data untouched).
std::vector<std::string> cleanup_partial_uploads(TelegramClient& tg, TeleVault& vault,
                                                 int64_t channel_id, bool dry_run = true);

} // namespace tv
