#include "snapshot.hpp"
#include <spdlog/spdlog.h>

namespace tv {

class SnapshotManager::Impl {};

SnapshotManager::SnapshotManager() : impl_(std::make_unique<Impl>()) {}

bool SnapshotManager::save_index(int64_t) {
    spdlog::warn("SnapshotManager::save_index not yet implemented");
    return false;
}
bool SnapshotManager::load_index(int64_t) {
    spdlog::warn("SnapshotManager::load_index not yet implemented");
    return false;
}
bool SnapshotManager::save_snapshot(const Snapshot&, int64_t) {
    spdlog::warn("SnapshotManager::save_snapshot not yet implemented");
    return false;
}
Snapshot SnapshotManager::load_snapshot(int64_t) const {
    spdlog::warn("SnapshotManager::load_snapshot not yet implemented");
    return {};
}

} // namespace tv
