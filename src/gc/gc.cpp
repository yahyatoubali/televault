#include "gc.hpp"
#include "../telegram/client.hpp"
#include "../core/vault.hpp"
#include "../util/config.hpp"
#include "../models/snapshot.hpp"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <set>
#include <unordered_map>

namespace tv {

namespace {

// A text message is TeleVault-owned when it parses as file metadata,
// a snapshot, or a snapshot index. Anything else is user content and
// must never be auto-deleted.
bool message_type(const std::string& text, std::string& type_out) {
    if (text.empty() || text.front() != '{') return false;
    try {
        auto j = nlohmann::json::parse(text);
        if (j.contains("id") && j.contains("chunks") && j.contains("name")) {
            type_out = "metadata";
            return true;
        }
        std::string t = j.value("type", "");
        if (t == "snapshot") {
            type_out = "snapshot";
            return true;
        }
        if (t == "snapshot_index") {
            type_out = "snapshot_index";
            return true;
        }
    } catch (...) {}
    return false;
}

} // namespace

std::vector<OrphanInfo> find_orphans(
    const std::vector<std::pair<int64_t, std::string>>& history,
    const std::unordered_set<int64_t>& referenced) {
    std::vector<OrphanInfo> out;
    for (auto& [id, text] : history) {
        if (referenced.count(normalize_message_id(id))) continue;
        std::string type;
        if (message_type(text, type)) {
            out.push_back({id, type, 0});
        } else if (text.empty()) {
            out.push_back({id, "file_chunk", 0});
        }
        // Non-empty, non-JSON text: unknown user content — leave alone.
    }
    return out;
}

GarbageCollectionResult collect_garbage(TelegramClient& tg, TeleVault& vault,
                                        int64_t channel_id, bool dry_run) {
    GarbageCollectionResult result;
    spdlog::info("Scanning channel {} for orphaned messages...", channel_id);

    // 1. Build the referenced set (canonical IDs).
    std::unordered_set<int64_t> referenced;
    auto mark = [&](int64_t id) {
        if (id != 0) referenced.insert(normalize_message_id(id));
    };

    mark(tg.get_pinned_message_id(channel_id));

    for (auto& [fid, mid] : vault.index_entries()) {
        mark(mid);
        auto meta = vault.get_metadata_by_id(mid);
        if (!meta) {
            // Do NOT treat its chunks as orphans: a transient fetch failure
            // must never cause live data to be deleted. Handled by partial
            // cleanup; collect refuses --force while any exist (below).
            ++result.unresolved_index_entries;
            continue;
        }
        mark(meta->metadata_message_id);
        for (auto& c : meta->chunks) {
            if (c.channel_id == 0 || c.channel_id == channel_id) mark(c.message_id);
        }
    }

    // Snapshot side: index message + every snapshot metadata message.
    int64_t snap_idx = ConfigManager::instance().get().snapshot_index_msg_id;
    mark(snap_idx);
    if (snap_idx != 0) {
        auto msg = tg.get_message(channel_id, snap_idx);
        if (!msg.text.empty()) {
            try {
                auto idx = nlohmann::json::parse(msg.text).get<SnapshotIndex>();
                for (auto& [sid, smid] : idx.snapshots) mark(smid);
            } catch (const std::exception& e) {
                spdlog::warn("Cannot parse snapshot index: {}", e.what());
            }
        }
    }

    // 2. Page channel history.
    constexpr int kHistoryCap = 20000;
    auto history = tg.get_chat_history(channel_id, 0, kHistoryCap);
    result.scanned_messages = history.size();
    if (history.size() >= static_cast<size_t>(kHistoryCap)) {
        spdlog::warn("History hit the {} message scan cap; orphans beyond it are unchecked", kHistoryCap);
    }

    std::vector<std::pair<int64_t, std::string>> simple;
    simple.reserve(history.size());
    for (auto& m : history) simple.emplace_back(m.id, m.text);

    // 3. Classify. Empty-text candidates must actually carry a document —
    // verify via get_message so service messages are never deleted.
    auto candidates = find_orphans(simple, referenced);
    for (auto& c : candidates) {
        if (c.type == "file_chunk") {
            auto full = tg.get_message(channel_id, c.message_id);
            if (full.id == 0 || full.file_id == 0) continue; // not a document; leave it
        }
        result.orphans.push_back(c);
    }

    if (dry_run) {
        spdlog::info("Dry-run: found {} orphan(s) in {} scanned message(s) — use --force to delete",
                     result.orphans.size(), result.scanned_messages);
        if (result.unresolved_index_entries > 0) {
            spdlog::warn("{} indexed file(s) could not be loaded; resolve them (see gc --clean-partials) before any --force run",
                         result.unresolved_index_entries);
        }
        return result;
    }

    // Fail-safe: never delete when the referenced set may be incomplete.
    if (result.unresolved_index_entries > 0) {
        spdlog::error("Refusing --force: {} indexed file(s) failed to load (transient network or "
                      "broken entries). Run 'gc --clean-partials' first, then retry.",
                      result.unresolved_index_entries);
        result.orphans.clear();
        return result;
    }

    // Fail-safe 2: an orphan metadata whose file id is STILL indexed under
    // the same message must be a classifier bug — abort rather than delete.
    {
        std::unordered_map<std::string, int64_t> index_mid;
        for (auto& [fid, mid] : vault.index_entries()) index_mid[fid] = mid;
        for (auto& o : result.orphans) {
            if (o.type != "metadata") continue;
            auto full = tg.get_message(channel_id, o.message_id);
            if (full.text.empty()) continue;
            try {
                auto j = nlohmann::json::parse(full.text);
                std::string fid = j.value("id", "");
                auto it = index_mid.find(fid);
                if (it != index_mid.end() &&
                    normalize_message_id(it->second) == normalize_message_id(o.message_id)) {
                    spdlog::error("Refusing --force: orphan candidate {} is still indexed (classifier bug?)",
                                  o.message_id);
                    result.orphans.clear();
                    return result;
                }
            } catch (...) {}
        }
    }

    // 4. Delete in batches. Snapshot/snapshot_index orphans are REPORTED
    // ONLY: the snapshot index is the sole registry of backups, and an
    // apparently-unreferenced snapshot may belong to an index message we
    // could not load (e.g. config loss). Deleting it would destroy backups
    // `backup list` can no longer see but `recover`-style discovery could.
    constexpr size_t kBatch = 100;
    size_t deleted = 0, skipped = 0;
    for (size_t i = 0; i < result.orphans.size(); i += kBatch) {
        std::vector<int64_t> batch;
        for (size_t j = i; j < std::min(result.orphans.size(), i + kBatch); ++j) {
            const auto& o = result.orphans[j];
            if (o.type == "snapshot" || o.type == "snapshot_index") {
                ++skipped;
                spdlog::warn("Keeping orphan {} {} (snapshots are never auto-deleted)", o.type, o.message_id);
                continue;
            }
            batch.push_back(o.message_id);
        }
        if (batch.empty()) continue;
        if (tg.delete_messages(channel_id, batch)) deleted += batch.size();
        else spdlog::warn("Failed to delete orphan batch at offset {}", i);
    }
    spdlog::info("Deleted {}/{} orphan message(s) ({} snapshot message(s) kept)",
                 deleted, result.orphans.size(), skipped);
    return result;
}

std::vector<std::string> cleanup_partial_uploads(TelegramClient& tg, TeleVault& vault,
                                                 int64_t channel_id, bool dry_run) {
    (void)tg;
    (void)channel_id;
    std::vector<std::string> partials;
    for (auto& [fid, mid] : vault.index_entries()) {
        auto meta = vault.get_metadata_by_id(mid);
        if (!meta || meta->chunks.empty()) partials.push_back(fid);
    }
    if (partials.empty()) {
        spdlog::info("No partial uploads found");
        return partials;
    }
    spdlog::warn("Found {} partial/broken index entr{}: ", partials.size(),
                 partials.size() == 1 ? "y" : "ies");
    for (auto& fid : partials) spdlog::warn("  {}", fid);
    if (dry_run) {
        spdlog::info("Dry-run — re-run with --force to drop them from the local index");
        return partials;
    }
    size_t dropped = 0;
    for (auto& fid : partials) {
        if (vault.remove_index_entry(fid)) ++dropped;
    }
    spdlog::info("Dropped {}/{} partial index entr{}", dropped, partials.size(),
                 dropped == 1 ? "y" : "ies");
    return partials;
}

} // namespace tv
