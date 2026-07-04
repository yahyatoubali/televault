#include "config.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace tv {

ConfigManager& ConfigManager::instance() {
    static ConfigManager inst;
    return inst;
}

ConfigManager::ConfigManager() {
    config_.config_dir = resolve_xdg("XDG_CONFIG_HOME", ".config");
    config_.data_dir = resolve_xdg("XDG_DATA_HOME", ".local/share");
    config_path_ = config_.config_dir + "/televault/config.json";
}

std::string ConfigManager::resolve_xdg(const char* env, const char* fallback) const {
    if (auto* val = std::getenv(env)) return val;
    if (auto* home = std::getenv("HOME")) return std::string(home) + "/" + fallback;
    return "/tmp";
}

bool ConfigManager::load() {
    std::ifstream f(config_path_);
    if (!f.is_open()) return false;
    try {
        nlohmann::json j;
        f >> j;
        config_ = j.template get<Config>();
        return true;
    } catch (...) {
        return false;
    }
}

void ConfigManager::save() const {
    auto dir = std::filesystem::path(config_path_).parent_path();
    std::filesystem::create_directories(dir);

    auto tmp = config_path_ + ".tmp";
    {
        std::ofstream f(tmp);
        nlohmann::json j = config_;
        f << j.dump(2);
        f.flush();
    }
    std::filesystem::rename(tmp, config_path_);
}

std::string ConfigManager::config_dir() const {
    return config_.config_dir + "/televault";
}

std::string ConfigManager::data_dir() const {
    return config_.data_dir + "/televault";
}

} // namespace tv
