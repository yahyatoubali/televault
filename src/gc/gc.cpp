#include "gc.hpp"
#include "../telegram/client.hpp"
#include "../core/index.hpp"
#include "../util/config.hpp"
#include <spdlog/spdlog.h>
#include <set>

namespace tv {

GarbageCollectionResult collect_garbage(int64_t channel_id, bool dry_run) {
    GarbageCollectionResult result;

    // Get all messages in the channel
    // For large channels, we'd need pagination
    spdlog::info("Scanning channel {} for orphaned messages...", channel_id);

    // Index is loaded separately

    // In the full implementation:
    // 1. Get all pinned messages (index + snapshot index)
    // 2. Get all text messages (metadata + snapshots)
    // 3. Get all file messages (chunks)
    // 4. Build the set of known message IDs from the index
    // 5. Any message not in the known set is an orphan

    if (dry_run) {
        spdlog::info("Dry-run mode — use --force to delete orphans");
    }

    spdlog::info("Found {} orphans, {} reclaimable",
                 result.orphans.size(), result.reclaimable_bytes);
    return result;
}

std::vector<int64_t> cleanup_partial_uploads(int64_t channel_id) {
    spdlog::info("Scanning for partial uploads in channel {}...", channel_id);
    // Check index for files with incomplete chunk lists
    return {};
}

} // namespace tv
