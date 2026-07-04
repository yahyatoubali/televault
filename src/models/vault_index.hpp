#pragma once

#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace tv {

struct VaultIndex {
    int64_t version{1};
    std::unordered_map<std::string, int64_t> files;

    static constexpr int64_t CURRENT_VERSION = 1;
};

inline void to_json(nlohmann::json& j, const VaultIndex& idx) {
    j = nlohmann::json{
        {"version", idx.version},
        {"files", idx.files}
    };
}

inline void from_json(const nlohmann::json& j, VaultIndex& idx) {
    j.at("version").get_to(idx.version);
    j.at("files").get_to(idx.files);
}

} // namespace tv
