#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "models/file_metadata.hpp"

namespace tv {

struct SnapshotFile {
    std::string path; // Relative path from snapshot root
    std::string file_id;
    std::string name; // Backward compat alias for path
    uint64_t size{0};
    std::string hash;
    std::chrono::system_clock::time_point modified_at{std::chrono::system_clock::now()};
    bool incremental{false};
};

inline void to_json(nlohmann::json& j, const SnapshotFile& f) {
    std::string p = !f.path.empty() ? f.path : f.name;
    j = nlohmann::json{
        {"path", p},
        {"file_id", f.file_id},
        {"size", f.size},
        {"hash", f.hash},
        {"modified_at", detail::timestamp_to_double(f.modified_at)}
    };
    if (!f.name.empty()) {
        j["name"] = f.name;
    }
    if (f.incremental) {
        j["incremental"] = true;
    }
}

inline void from_json(const nlohmann::json& j, SnapshotFile& f) {
    if (j.contains("path") && !j["path"].is_null()) {
        f.path = j["path"].get<std::string>();
        f.name = f.path;
    } else if (j.contains("name") && !j["name"].is_null()) {
        f.name = j["name"].get<std::string>();
        f.path = f.name;
    } else {
        f.path.clear();
        f.name.clear();
    }

    if (j.contains("file_id") && !j["file_id"].is_null()) {
        f.file_id = j["file_id"].get<std::string>();
    } else {
        f.file_id.clear();
    }

    if (j.contains("size") && !j["size"].is_null()) {
        f.size = j["size"].get<uint64_t>();
    } else {
        f.size = 0;
    }

    if (j.contains("hash") && !j["hash"].is_null()) {
        f.hash = j["hash"].get<std::string>();
    } else {
        f.hash.clear();
    }

    if (j.contains("modified_at") && !j["modified_at"].is_null()) {
        f.modified_at = detail::parse_timestamp(j["modified_at"]);
    } else {
        f.modified_at = std::chrono::system_clock::now();
    }

    if (j.contains("incremental") && !j["incremental"].is_null()) {
        f.incremental = j["incremental"].get<bool>();
    } else {
        f.incremental = false;
    }
}

struct Snapshot {
    std::string id;
    std::string name;
    std::chrono::system_clock::time_point created_at{std::chrono::system_clock::now()};
    std::string source_path;
    uint64_t file_count{0};
    uint64_t total_size{0};
    uint64_t stored_size{0};
    bool encrypted{true};
    bool compressed{false};
    std::optional<std::string> parent_id;
    std::vector<SnapshotFile> files;
    std::optional<int64_t> message_id;

    [[nodiscard]] bool is_incremental() const noexcept { return parent_id.has_value(); }
};

inline void to_json(nlohmann::json& j, const Snapshot& s) {
    j = nlohmann::json{
        {"id", s.id},
        {"name", s.name},
        {"type", "snapshot"},
        {"created_at", detail::timestamp_to_double(s.created_at)},
        {"source_path", s.source_path},
        {"file_count", s.file_count > 0 ? s.file_count : s.files.size()},
        {"total_size", s.total_size},
        {"stored_size", s.stored_size},
        {"encrypted", s.encrypted},
        {"compressed", s.compressed},
        {"files", s.files}
    };
    if (s.parent_id.has_value()) {
        j["parent_id"] = *s.parent_id;
    }
    if (s.message_id.has_value()) {
        j["message_id"] = *s.message_id;
    }
}

inline void from_json(const nlohmann::json& j, Snapshot& s) {
    j.at("id").get_to(s.id);
    j.at("name").get_to(s.name);

    if (j.contains("created_at") && !j["created_at"].is_null()) {
        s.created_at = detail::parse_timestamp(j["created_at"]);
    } else {
        s.created_at = std::chrono::system_clock::now();
    }

    if (j.contains("source_path") && !j["source_path"].is_null()) {
        s.source_path = j["source_path"].get<std::string>();
    } else {
        s.source_path.clear();
    }

    if (j.contains("file_count") && !j["file_count"].is_null()) {
        s.file_count = j["file_count"].get<uint64_t>();
    } else {
        s.file_count = 0;
    }

    if (j.contains("total_size") && !j["total_size"].is_null()) {
        s.total_size = j["total_size"].get<uint64_t>();
    } else {
        s.total_size = 0;
    }

    if (j.contains("stored_size") && !j["stored_size"].is_null()) {
        s.stored_size = j["stored_size"].get<uint64_t>();
    } else {
        s.stored_size = 0;
    }

    if (j.contains("encrypted") && !j["encrypted"].is_null()) {
        s.encrypted = j["encrypted"].get<bool>();
    } else {
        s.encrypted = true;
    }

    if (j.contains("compressed") && !j["compressed"].is_null()) {
        s.compressed = j["compressed"].get<bool>();
    } else {
        s.compressed = false;
    }

    if (j.contains("parent_id") && !j["parent_id"].is_null()) {
        s.parent_id = j["parent_id"].get<std::string>();
    } else {
        s.parent_id = std::nullopt;
    }

    if (j.contains("message_id") && !j["message_id"].is_null()) {
        s.message_id = j["message_id"].get<int64_t>();
    } else {
        s.message_id = std::nullopt;
    }

    if (j.contains("files") && j["files"].is_array()) {
        s.files = j["files"].get<std::vector<SnapshotFile>>();
    } else {
        s.files.clear();
    }

    if (s.file_count == 0) {
        s.file_count = s.files.size();
    }
}

struct SnapshotIndex {
    int64_t version{2};
    std::unordered_map<std::string, int64_t> snapshots; // snapshot_id -> message_id
    double updated_at{0.0};
};

inline void to_json(nlohmann::json& j, const SnapshotIndex& idx) {
    j = nlohmann::json{
        {"type", "snapshot_index"},
        {"version", idx.version},
        {"snapshots", idx.snapshots}
    };
    if (idx.updated_at > 0.0) {
        j["updated_at"] = idx.updated_at;
    }
}

inline void from_json(const nlohmann::json& j, SnapshotIndex& idx) {
    if (j.contains("version") && !j["version"].is_null()) {
        idx.version = j["version"].get<int64_t>();
    } else {
        idx.version = 2;
    }

    idx.snapshots.clear();
    if (j.contains("snapshots") && j["snapshots"].is_object()) {
        for (const auto& [key, val] : j["snapshots"].items()) {
            if (!val.is_null() && val.is_number_integer()) {
                idx.snapshots[key] = val.get<int64_t>();
            }
        }
    }

    if (j.contains("updated_at") && !j["updated_at"].is_null()) {
        idx.updated_at = j["updated_at"].get<double>();
    } else {
        idx.updated_at = 0.0;
    }
}

struct RetentionPolicy {
    int keep_daily{7};
    int keep_weekly{4};
    int keep_monthly{6};
    bool keep_all{false};

    // Aliases for backward compatibility
    int daily{7};
    int weekly{4};
    int monthly{6};
};

inline void to_json(nlohmann::json& j, const RetentionPolicy& r) {
    j = nlohmann::json{
        {"keep_daily", r.keep_daily},
        {"keep_weekly", r.keep_weekly},
        {"keep_monthly", r.keep_monthly},
        {"keep_all", r.keep_all},
        {"daily", r.keep_daily},
        {"weekly", r.keep_weekly},
        {"monthly", r.keep_monthly}
    };
}

inline void from_json(const nlohmann::json& j, RetentionPolicy& r) {
    if (j.contains("keep_daily") && !j["keep_daily"].is_null()) {
        r.keep_daily = j["keep_daily"].get<int>();
    } else if (j.contains("daily") && !j["daily"].is_null()) {
        r.keep_daily = j["daily"].get<int>();
    } else {
        r.keep_daily = 7;
    }

    if (j.contains("keep_weekly") && !j["keep_weekly"].is_null()) {
        r.keep_weekly = j["keep_weekly"].get<int>();
    } else if (j.contains("weekly") && !j["weekly"].is_null()) {
        r.keep_weekly = j["weekly"].get<int>();
    } else {
        r.keep_weekly = 4;
    }

    if (j.contains("keep_monthly") && !j["keep_monthly"].is_null()) {
        r.keep_monthly = j["keep_monthly"].get<int>();
    } else if (j.contains("monthly") && !j["monthly"].is_null()) {
        r.keep_monthly = j["monthly"].get<int>();
    } else {
        r.keep_monthly = 6;
    }

    if (j.contains("keep_all") && !j["keep_all"].is_null()) {
        r.keep_all = j["keep_all"].get<bool>();
    } else {
        r.keep_all = false;
    }

    r.daily = r.keep_daily;
    r.weekly = r.keep_weekly;
    r.monthly = r.keep_monthly;
}

} // namespace tv
