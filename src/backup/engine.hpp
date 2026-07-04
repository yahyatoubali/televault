#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "../models/snapshot.hpp"
#include "../core/vault.hpp"

namespace tv {

class BackupEngine {
public:
    BackupEngine(TeleVault& vault, TelegramClient& tg);
    ~BackupEngine();

    bool create_snapshot(const std::string& name,
                         const std::vector<std::string>& paths,
                         const std::string& password,
                         bool incremental,
                         ProgressCallback cb = {});

    bool restore_snapshot(const std::string& snapshot_id,
                          const std::string& output_dir,
                          const std::string& password,
                          ProgressCallback cb = {});

    std::vector<Snapshot> list_snapshots() const;
    bool delete_snapshot(const std::string& snapshot_id);
    bool prune_snapshots(const RetentionPolicy& policy);
    bool verify_snapshot(const std::string& snapshot_id, const std::string& password);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv
