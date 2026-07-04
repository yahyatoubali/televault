#include "session.hpp"
#include "../util/config.hpp"
#include <cstdlib>
#include <filesystem>

namespace tv {

SessionManager::SessionManager() {
    auto base = resolve_data_dir();
    db_dir_ = base + "/tdlib";
    files_dir_ = db_dir_ + "/files";

    auto& cfg = ConfigManager::instance();
    api_id_ = cfg.get().telegram.api_id;
    api_hash_ = cfg.get().telegram.api_hash;
    phone_ = cfg.get().telegram.phone;
}

std::string SessionManager::resolve_data_dir() {
    if (auto* home = std::getenv("XDG_DATA_HOME")) {
        return std::string(home) + "/televault";
    }
    if (auto* home = std::getenv("HOME")) {
        return std::string(home) + "/.local/share/televault";
    }
    return "/tmp/televault";
}

std::string SessionManager::db_dir() const {
    std::filesystem::create_directories(db_dir_);
    return db_dir_;
}

std::string SessionManager::files_dir() const {
    std::filesystem::create_directories(files_dir_);
    return files_dir_;
}

bool SessionManager::has_api_credentials() const {
    return api_id_ > 0 && !api_hash_.empty();
}

bool SessionManager::save_api_credentials(int32_t api_id, const std::string& api_hash, const std::string& phone) {
    api_id_ = api_id;
    api_hash_ = api_hash;
    phone_ = phone;

    auto& cfg = ConfigManager::instance();
    auto config = cfg.get();
    config.telegram.api_id = api_id;
    config.telegram.api_hash = api_hash;
    config.telegram.phone = phone;
    cfg.set(config);
    cfg.save();
    return true;
}

void SessionManager::clear() {
    api_id_ = 0;
    api_hash_.clear();
    phone_.clear();
    auto& cfg = ConfigManager::instance();
    auto config = cfg.get();
    config.telegram = {};
    cfg.set(config);
    cfg.save();
    std::filesystem::remove_all(db_dir_);
}

} // namespace tv
