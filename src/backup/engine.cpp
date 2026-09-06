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
        auto& cfg = ConfigManager::instance().get();
        int64_t msg_id = cfg.snapshot_index_msg_id;

        if (msg_id != 0) {
            auto msg = tg.get_message(chat_id, msg_id);
            if (!msg.text.empty()) {
                try {
                    auto j = nlohmann::json::parse(msg.text);
                    if (j.value("type", "") == "snapshot_index") {
                        return j.get<SnapshotIndex>();
                    }
                } catch (...) {}
            }
        }

        // Search recent history for snapshot_index if not in config
        auto history = tg.get_chat_history(chat_id, 0, 50);
        for (const auto& m : history) {
            if (m.text.empty()) continue;
            try {
                auto j = nlohmann::json::parse(m.text);
                if (j.value("type", "") == "snapshot_index") {
                    auto cfg_copy = ConfigManager::instance().get();
                    cfg_copy.snapshot_index_msg_id = m.id;
                    ConfigManager::instance().set(cfg_copy);
                    ConfigManager::instance().save();
                    return j.get<SnapshotIndex>();
                }
            } catch (...) {}
        }

        return {};
    }

    bool save_index(const SnapshotIndex& idx) {
        auto chat_id = get_channel_id();
        auto json_str = nlohmann::json(idx).dump();
        auto& cfg = ConfigManager::instance().get();
        int64_t msg_id = cfg.snapshot_index_msg_id;

        bool saved = false;
        if (msg_id != 0) {
            saved = tg.edit_message(chat_id, msg_id, json_str);
        }
        if (!saved) {
            auto new_id = tg.send_text(chat_id, json_str);
            if (new_id > 0) {
                auto cfg_copy = ConfigManager::instance().get();
                cfg_copy.snapshot_index_msg_id = new_id;
                ConfigManager::instance().set(cfg_copy);
                ConfigManager::instance().save();
                return true;
            }
            return false;
        }
        return true;
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
        snap.id = std::format("{:x}", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
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
            auto process_file = [&](const std::string& file_path, const std::string& rel_path) {
                if (cb) {
                    cb({static_cast<uint64_t>(processed),
                        static_cast<uint64_t>(total_files),
                        std::format("Backing up {}...", std::filesystem::path(file_path).filename().string()),
                        0});
                }

                std::string fname = std::filesystem::path(file_path).filename().string();
                FileEntry existing;
                // Check if file already in vault for incremental
                if (incremental) {
                    auto files = vault.list_files();
                    for (auto& f : files) {
                        if (f.name == fname) {
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
                sf.name = fname;
                sf.path = rel_path;
                sf.size = std::filesystem::file_size(file_path);
                sf.incremental = incremental && !existing.name.empty();
                if (existing.name.empty()) {
                    auto info = vault.get_file_info(fname);
                    if (info) {
                        sf.file_id = info->id;
                    }
                } else {
                    sf.file_id = existing.id;
                }
                snap.files.push_back(std::move(sf));
                ++processed;
            };

            if (std::filesystem::is_directory(base_path)) {
                for (auto& entry : std::filesystem::recursive_directory_iterator(base_path)) {
                    if (entry.is_regular_file()) {
                        std::string rel = std::filesystem::relative(entry.path(), base_path).string();
                        process_file(entry.path().string(), rel);
                    }
                }
            } else {
                std::string rel = std::filesystem::path(base_path).filename().string();
                process_file(base_path, rel);
            }
        }

        uint64_t total_bytes = 0;
        for (const auto& sf : snap.files) {
            total_bytes += sf.size;
        }
        snap.file_count = snap.files.size();
        snap.total_size = total_bytes;
        snap.stored_size = total_bytes;
        snap.encrypted = !password.empty();

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
            std::string file_rel = !sf.path.empty() ? sf.path : sf.name;
            if (cb) {
                cb({static_cast<uint64_t>(i),
                    static_cast<uint64_t>(snap->files.size()),
                    std::format("Restoring {}...", sf.name),
                    0});
            }

            // Sanitize path: prevent directory traversal (CWE-22)
            std::filesystem::path sanitized = std::filesystem::path(file_rel).relative_path();
            auto output = std::filesystem::weakly_canonical(
                std::filesystem::absolute(output_dir) / sanitized);
            if (output.string().find(std::filesystem::absolute(output_dir).string()) != 0) {
                spdlog::error("Path traversal detected in snapshot: {}", file_rel);
                continue;
            }
            std::filesystem::create_directories(output.parent_path());

            VaultOptions opts;
            opts.password = password;
            std::string pull_target = !sf.file_id.empty() ? sf.file_id : sf.name;
            if (!vault.pull(pull_target, output.string(), opts)) {
                spdlog::error("Failed to restore file: {}", sf.name);
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
