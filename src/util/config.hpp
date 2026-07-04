#pragma once

#include <string>
#include <optional>
#include "../models/config.hpp"

namespace tv {

class ConfigManager {
public:
    static ConfigManager& instance();

    bool load();
    void save() const;
    void save_telegram() const;

    [[nodiscard]] const Config& get() const { return config_; }
    void set(const Config& cfg) { config_ = cfg; }

    [[nodiscard]] std::string config_dir() const;
    [[nodiscard]] std::string data_dir() const;

private:
    ConfigManager();
    std::string resolve_xdg(const char* env, const char* fallback) const;

    Config config_;
    std::string config_path_;
};

} // namespace tv
