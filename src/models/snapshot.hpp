#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <nlohmann/json.hpp>

namespace tv {

struct SnapshotFile {
    std::string file_id;
    std::string name;
    uint64_t size{};
    std::string hash;
    std::chrono::system_clock::time_point modified_at;
    bool incremental{};
};

struct Snapshot {
    std::string id;
    std::string name;
    std::chrono::system_clock::time_point created_at;
    std::vector<SnapshotFile> files;
};

struct SnapshotIndex {
    std::unordered_map<std::string, int64_t> snapshots; // snapshot_id → message_id
};

struct RetentionPolicy {
    int daily{7};
    int weekly{4};
    int monthly{3};
};

inline void to_json(nlohmann::json& j, const SnapshotFile& f) {
    j = nlohmann::json{
        {"file_id", f.file_id},
        {"name", f.name},
        {"size", f.size},
        {"hash", f.hash},
        {"modified_at", std::chrono::duration_cast<std::chrono::seconds>(
                            f.modified_at.time_since_epoch()).count()},
        {"incremental", f.incremental}
    };
}

inline void from_json(const nlohmann::json& j, SnapshotFile& f) {
    j.at("file_id").get_to(f.file_id);
    j.at("name").get_to(f.name);
    j.at("size").get_to(f.size);
    j.at("hash").get_to(f.hash);
    int64_t ts{};
    j.at("modified_at").get_to(ts);
    f.modified_at = std::chrono::system_clock::from_time_t(ts);
    j.at("incremental").get_to(f.incremental);
}

inline void to_json(nlohmann::json& j, const Snapshot& s) {
    j = nlohmann::json{
        {"id", s.id},
        {"name", s.name},
        {"created_at", std::chrono::duration_cast<std::chrono::seconds>(
                           s.created_at.time_since_epoch()).count()},
        {"files", s.files}
    };
}

inline void from_json(const nlohmann::json& j, Snapshot& s) {
    j.at("id").get_to(s.id);
    j.at("name").get_to(s.name);
    int64_t ts{};
    j.at("created_at").get_to(ts);
    s.created_at = std::chrono::system_clock::from_time_t(ts);
    j.at("files").get_to(s.files);
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SnapshotIndex, snapshots)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RetentionPolicy, daily, weekly, monthly)

} // namespace tv
