#include "config.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    auto saved_config_dir = config_.config_dir;
    auto saved_data_dir = config_.data_dir;

    // Load main config
    {
        std::ifstream f(config_path_);
        if (f.is_open()) {
            try {
                nlohmann::json j;
                f >> j;

                // Backward compat: convert flat retry fields to nested
                if (!j.contains("retry") && j.contains("max_retries")) {
                    nlohmann::json r;
                    r["max_retries"] = (!j["max_retries"].is_null()) ? j["max_retries"].get<int>() : 5;
                    double rd = (j.contains("retry_delay") && !j["retry_delay"].is_null()) ? j["retry_delay"].get<double>() : 1.0;
                    r["base_delay_ms"] = static_cast<int>(rd * 1000);
                    r["max_delay_ms"] = r["base_delay_ms"];
                    r["jitter_factor"] = 0.1;
                    j["retry"] = std::move(r);
                }
                // Backward compat: convert flat low_resource fields to nested
                if (!j.contains("low_resource") && j.contains("low_resource_mode")) {
                    nlohmann::json lr;
                    lr["enabled"] = (!j["low_resource_mode"].is_null()) ? j["low_resource_mode"].get<bool>() : false;
                    lr["chunk_size"] = (j.contains("low_resource_chunk_size") && !j["low_resource_chunk_size"].is_null()) ? j["low_resource_chunk_size"].get<uint64_t>() : 33554432;
                    int par = (j.contains("low_resource_parallelism") && !j["low_resource_parallelism"].is_null()) ? j["low_resource_parallelism"].get<int>() : 2;
                    lr["parallel_uploads"] = par;
                    lr["parallel_downloads"] = par;
                    lr["hasher_threads"] = (j.contains("low_resource_hash_workers") && !j["low_resource_hash_workers"].is_null()) ? j["low_resource_hash_workers"].get<int>() : 1;
                    lr["sequential_download"] = true;
                    lr["max_no_compress_size"] = 524288000;
                    j["low_resource"] = std::move(lr);
                }

                // Backward compat: ensure telegram field exists
                if (!j.contains("telegram")) {
                    j["telegram"] = nlohmann::json{{"api_id", 0}, {"api_hash", ""}, {"phone", ""}};
                }

                config_ = j.template get<Config>();
            } catch (...) {
                config_.config_dir = saved_config_dir;
                config_.data_dir = saved_data_dir;
                return false;
            }
        }
    }

    // Restore config/data dirs (not serialized in JSON)
    config_.config_dir = saved_config_dir;
    config_.data_dir = saved_data_dir;

    // Load telegram credentials from telegram.json
    {
        auto tg_path = config_dir() + "/telegram.json";
        std::ifstream tg_f(tg_path);
        if (tg_f.is_open()) {
            try {
                nlohmann::json j;
                tg_f >> j;
                if (j.contains("api_id") && !j["api_id"].is_null()) config_.telegram.api_id = j["api_id"].get<int32_t>();
                if (j.contains("api_hash") && !j["api_hash"].is_null()) config_.telegram.api_hash = j["api_hash"].get<std::string>();
            } catch (...) {}
        }
    }

    return true;
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

void ConfigManager::save_telegram() const {
    auto dir = config_dir();
    std::filesystem::create_directories(dir);
    auto path = dir + "/telegram.json";
    auto tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        nlohmann::json j;
        j["api_id"] = config_.telegram.api_id;
        j["api_hash"] = config_.telegram.api_hash;
        f << j.dump(2);
        f.flush();
    }
    std::filesystem::rename(tmp, path);
}

std::string ConfigManager::config_dir() const {
    return config_.config_dir + "/televault";
}

std::string ConfigManager::data_dir() const {
    return config_.data_dir + "/televault";
}

} // namespace tv
