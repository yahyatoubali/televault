#pragma once

#include <string>
#include <unordered_map>
#include <chrono>
#include <optional>
#include <nlohmann/json.hpp>

namespace tv {

struct VaultIndex {
    int64_t version{1};
    std::unordered_map<std::string, int64_t> files;
    double updated_at{0.0};

    static constexpr int64_t CURRENT_VERSION = 1;

    void add_file(const std::string& file_id, int64_t message_id) {
        files[file_id] = message_id;
        updated_at = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::optional<int64_t> remove_file(const std::string& file_id) {
        auto it = files.find(file_id);
        if (it != files.end()) {
            int64_t msg_id = it->second;
            files.erase(it);
            updated_at = std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            return msg_id;
        }
        return std::nullopt;
    }
};

inline void to_json(nlohmann::json& j, const VaultIndex& idx) {
    j = nlohmann::json{
        {"version", idx.version},
        {"files", idx.files}
    };
    if (idx.updated_at > 0.0) {
        j["updated_at"] = idx.updated_at;
    }
}

inline void from_json(const nlohmann::json& j, VaultIndex& idx) {
    if (j.contains("version") && !j["version"].is_null()) {
        idx.version = j["version"].get<int64_t>();
    } else {
        idx.version = 1;
    }

    idx.files.clear();
    if (j.contains("files") && j["files"].is_object()) {
        for (const auto& [key, val] : j["files"].items()) {
            if (!val.is_null() && val.is_number_integer()) {
                idx.files[key] = val.get<int64_t>();
            }
        }
    }

    if (j.contains("updated_at") && !j["updated_at"].is_null()) {
        idx.updated_at = j["updated_at"].get<double>();
    } else {
        idx.updated_at = 0.0;
    }
}

} // namespace tv
