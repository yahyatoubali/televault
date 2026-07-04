#pragma once

#include <string>
#include <memory>
#include "../models/snapshot.hpp"

namespace tv {

class SnapshotManager {
public:
    SnapshotManager();

    bool save_index(int64_t channel_id);
    bool load_index(int64_t pinned_message_id);
    bool save_snapshot(const Snapshot& snap, int64_t channel_id);
    Snapshot load_snapshot(int64_t message_id) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv
