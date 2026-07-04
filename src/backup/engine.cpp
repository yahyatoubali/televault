#include "engine.hpp"
#include "../telegram/client.hpp"
#include "../core/vault.hpp"
#include "../util/config.hpp"
#include "../util/format.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <chrono>
#include <set>

namespace tv {

class BackupEngine::Impl {
public:
    TeleVault& vault;
    TelegramClient& tg;

    Impl(TeleVault& v, TelegramClient& c) : vault(v), tg(c) {}

    int64_t get_channel_id() const {
        return ConfigManager::instance().get().channel_id;
    }

    // ── Snapshot index management ──────────────────────────────────
    SnapshotIndex load_index() const {
        auto chat_id = get_channel_id();
        auto pinned = tg.get_pinned_message_id(chat_id);
        if (pinned == 0) return {};

        auto msg = tg.get_message(chat_id, pinned);
        if (msg.text.empty()) return {};

        try {
            return nlohmann::json::parse(msg.text).get<SnapshotIndex>();
        } catch (...) {
            return {};
        }
    }

    bool save_index(const SnapshotIndex& idx) {
        auto chat_id = get_channel_id();
        auto json_str = nlohmann::json(idx).dump();
        auto pinned = tg.get_pinned_message_id(chat_id);
        if (pinned == 0) {
            auto new_id = tg.send_text(chat_id, json_str);
            return new_id > 0 && tg.pin_message(chat_id, new_id);
        }
        return tg.edit_message(chat_id, pinned, json_str);
    }

    // ── Snapshot CRUD ──────────────────────────────────────────────
    bool save_snapshot(const Snapshot& snap) {
        auto chat_id = get_channel_id();
        auto json_str = nlohmann::json(snap).dump();
        auto msg_id = tg.send_text(chat_id, json_str);
        if (msg_id == 0) return false;

        auto idx = load_index();
        idx.snapshots[snap.id] = msg_id;
        return save_index(idx);
    }

    std::optional<Snapshot> load_snapshot(int64_t msg_id) const {
        auto chat_id = get_channel_id();
        auto msg = tg.get_message(chat_id, msg_id);
        if (msg.text.empty()) return std::nullopt;
        try {
            return nlohmann::json::parse(msg.text).get<Snapshot>();
        } catch (...) {
            return std::nullopt;
        }
    }

    bool delete_snapshot_msg(const std::string& snapshot_id) {
        auto idx = load_index();
        auto it = idx.snapshots.find(snapshot_id);
        if (it == idx.snapshots.end()) return false;

        tg.delete_messages(get_channel_id(), {it->second});
        idx.snapshots.erase(it);
        return save_index(idx);
    }

    // ── Core operations ────────────────────────────────────────────
    bool create_snapshot(const std::string& name,
                         const std::vector<std::string>& paths,
                         const std::string& password,
                         bool incremental,
                         ProgressCallback cb)
    {
        Snapshot snap;
        snap.id = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        snap.name = name;
        snap.created_at = std::chrono::system_clock::now();

        size_t total_files = 0;
        for (auto& p : paths) {
            if (std::filesystem::is_directory(p)) {
                for (auto& entry : std::filesystem::recursive_directory_iterator(p)) {
                    if (entry.is_regular_file()) ++total_files;
                }
            } else {
                ++total_files;
            }
        }

        size_t processed = 0;

        for (auto& base_path : paths) {
            auto process_file = [&](const std::string& file_path) {
                if (cb) {
                    cb({static_cast<uint64_t>(processed),
                        static_cast<uint64_t>(total_files),
                        std::format("Backing up {}...", std::filesystem::path(file_path).filename().string()),
                        0});
                }

                FileEntry existing;
                // Check if file already in vault for incremental
                if (incremental) {
                    auto files = vault.list_files();
                    for (auto& f : files) {
                        if (f.name == file_path) {
                            existing = f;
                            break;
                        }
                    }
                }

                if (!existing.name.empty() && incremental) {
                    // File already backed up — just reference it
                    spdlog::debug("Skipping unchanged file: {}", file_path);
                } else {
                    // Upload new version
                    VaultOptions opts;
                    opts.password = password;
                    if (!vault.push(file_path, opts)) {
                        spdlog::error("Failed to backup: {}", file_path);
                        return;
                    }
                }

                SnapshotFile sf;
                sf.file_id = existing.name.empty() ? std::string{} : existing.id;
                sf.name = file_path;
                sf.size = std::filesystem::file_size(file_path);
                sf.incremental = incremental && !existing.name.empty();
                snap.files.push_back(std::move(sf));
                ++processed;
            };

            if (std::filesystem::is_directory(base_path)) {
                for (auto& entry : std::filesystem::recursive_directory_iterator(base_path)) {
                    if (entry.is_regular_file()) {
                        process_file(entry.path().string());
                    }
                }
            } else {
                process_file(base_path);
            }
        }

        bool ok = save_snapshot(snap);
        if (ok && cb) {
            cb({static_cast<uint64_t>(total_files),
                static_cast<uint64_t>(total_files), "Backup complete", 0});
        }
        return ok;
    }

    bool restore_snapshot(const std::string& snapshot_id,
                          const std::string& output_dir,
                          const std::string& password,
                          ProgressCallback cb)
    {
        auto idx = load_index();
        auto it = idx.snapshots.find(snapshot_id);
        if (it == idx.snapshots.end()) return false;

        auto snap = load_snapshot(it->second);
        if (!snap) return false;

        std::filesystem::create_directories(output_dir);

        for (size_t i = 0; i < snap->files.size(); ++i) {
            auto& sf = snap->files[i];
            if (cb) {
                cb({static_cast<uint64_t>(i),
                    static_cast<uint64_t>(snap->files.size()),
                    std::format("Restoring {}...", std::filesystem::path(sf.name).filename().string()),
                    0});
            }

            // Sanitize path: prevent directory traversal (CWE-22)
            std::filesystem::path sanitized = std::filesystem::path(sf.name).relative_path();
            auto output = std::filesystem::weakly_canonical(
                std::filesystem::absolute(output_dir) / sanitized);
            if (output.string().find(std::filesystem::absolute(output_dir).string()) != 0) {
                spdlog::error("Path traversal detected in snapshot: {}", sf.name);
                continue;
            }
            std::filesystem::create_directories(output.parent_path());

            if (!sf.incremental) {
                VaultOptions opts;
                opts.password = password;
                vault.pull(sf.name, output.string(), opts);
            }
        }

        if (cb) {
            cb({static_cast<uint64_t>(snap->files.size()),
                static_cast<uint64_t>(snap->files.size()), "Restore complete", 0});
        }
        return true;
    }

    std::vector<Snapshot> list_snapshots() const {
        std::vector<Snapshot> result;
        auto idx = load_index();
        for (auto& [id, msg_id] : idx.snapshots) {
            auto snap = load_snapshot(msg_id);
            if (snap) result.push_back(std::move(*snap));
        }
        return result;
    }

    bool delete_snapshot(const std::string& snapshot_id) {
        return delete_snapshot_msg(snapshot_id);
    }

    bool prune_snapshots(const RetentionPolicy& policy) {
        auto idx = load_index();
        auto now = std::chrono::system_clock::now();

        std::vector<std::pair<std::string, int64_t>> sorted;
        for (auto& [id, msg_id] : idx.snapshots) {
            sorted.emplace_back(id, msg_id);
        }
        std::ranges::sort(sorted, [](auto& a, auto& b) { return a.first < b.first; });

        // Group by age
        std::vector<Snapshot> all;
        for (auto& [id, msg_id] : sorted) {
            auto snap = load_snapshot(msg_id);
            if (snap) all.push_back(std::move(*snap));
        }

        // Simple retention: keep last N of each category
        // daily = last 7 days, weekly = last 4 weeks, monthly = last 3 months
        auto cutoff_daily = now - std::chrono::hours(24 * policy.daily);
        auto cutoff_weekly = now - std::chrono::hours(24 * 7 * policy.weekly);
        auto cutoff_monthly = now - std::chrono::hours(24 * 30 * policy.monthly);

        std::set<std::string> keep;
        for (auto& snap : all) {
            if (snap.created_at > cutoff_daily || snap.created_at > cutoff_weekly) {
                keep.insert(snap.id);
            }
            if (snap.created_at > cutoff_monthly) {
                keep.insert(snap.id);
            }
        }

        for (auto& [id, msg_id] : sorted) {
            if (!keep.contains(id)) {
                delete_snapshot_msg(id);
            }
        }
        return true;
    }

    bool verify_snapshot(const std::string& snapshot_id, const std::string& password) {
        auto idx = load_index();
        auto it = idx.snapshots.find(snapshot_id);
        if (it == idx.snapshots.end()) return false;

        auto snap = load_snapshot(it->second);
        if (!snap) return false;

        for (auto& sf : snap->files) {
            if (!sf.incremental) {
                if (!vault.verify_file(sf.name)) {
                    spdlog::error("Verification failed: {}", sf.name);
                    return false;
                }
            }
        }
        return true;
    }
};

// ── Public API ────────────────────────────────────────────────────────
BackupEngine::BackupEngine(TeleVault& vault, TelegramClient& tg)
    : impl_(std::make_unique<Impl>(vault, tg)) {}
BackupEngine::~BackupEngine() = default;

bool BackupEngine::create_snapshot(const std::string& name,
                                   const std::vector<std::string>& paths,
                                   const std::string& password,
                                   bool incremental,
                                   ProgressCallback cb) {
    return impl_->create_snapshot(name, paths, password, incremental, std::move(cb));
}

bool BackupEngine::restore_snapshot(const std::string& id,
                                    const std::string& dir,
                                    const std::string& pw,
                                    ProgressCallback cb) {
    return impl_->restore_snapshot(id, dir, pw, std::move(cb));
}

std::vector<Snapshot> BackupEngine::list_snapshots() const {
    return impl_->list_snapshots();
}

bool BackupEngine::delete_snapshot(const std::string& id) {
    return impl_->delete_snapshot(id);
}

bool BackupEngine::prune_snapshots(const RetentionPolicy& policy) {
    return impl_->prune_snapshots(policy);
}

bool BackupEngine::verify_snapshot(const std::string& id, const std::string& pw) {
    return impl_->verify_snapshot(id, pw);
}

} // namespace tv
