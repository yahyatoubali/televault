#pragma once

#include <string>

namespace tv {

class SessionManager {
public:
    SessionManager();

    [[nodiscard]] std::string db_dir() const;
    [[nodiscard]] std::string files_dir() const;
    [[nodiscard]] bool has_api_credentials() const;
    [[nodiscard]] int32_t api_id() const { return api_id_; }
    [[nodiscard]] std::string api_hash() const { return api_hash_; }
    [[nodiscard]] std::string phone() const { return phone_; }

    bool save_api_credentials(int32_t api_id, const std::string& api_hash, const std::string& phone);
    void clear();

private:
    std::string db_dir_;
    std::string files_dir_;
    int32_t api_id_{};
    std::string api_hash_;
    std::string phone_;

    static std::string resolve_data_dir();
};

} // namespace tv
