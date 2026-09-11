#pragma once

#include <string>
#include <memory>
#include <vector>
#include <chrono>
#include <cctype>
#include <nlohmann/json.hpp>
#include "../models/config.hpp"

namespace tv {

class TeleVault;
class TelegramClient;

enum class Interval { Hourly, Daily, Weekly, Monthly };

[[nodiscard]] inline std::string interval_to_string(Interval i) {
    switch (i) {
        case Interval::Hourly: return "hourly";
        case Interval::Daily: return "daily";
        case Interval::Weekly: return "weekly";
        case Interval::Monthly: return "monthly";
    }
    return "daily";
}

[[nodiscard]] inline bool interval_from_string(const std::string& s, Interval& out) {
    std::string v = s;
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "hourly" || v == "hour" || v == "h") { out = Interval::Hourly; return true; }
    if (v == "daily" || v == "day" || v == "d") { out = Interval::Daily; return true; }
    if (v == "weekly" || v == "week" || v == "w") { out = Interval::Weekly; return true; }
    if (v == "monthly" || v == "month" || v == "m") { out = Interval::Monthly; return true; }
    return false;
}

struct ScheduleEntry {
    std::string name;
    std::string path;
    Interval interval{Interval::Daily};
    std::string password;
    bool incremental{true};
    std::chrono::system_clock::time_point last_run{};
    std::vector<std::string> exclude_patterns;
};

inline void to_json(nlohmann::json& j, const ScheduleEntry& e) {
    j = nlohmann::json{
        {"name", e.name},
        {"path", e.path},
        {"interval", interval_to_string(e.interval)},
        {"password", e.password},
        {"incremental", e.incremental},
        {"last_run", std::chrono::duration<double>(e.last_run.time_since_epoch()).count()},
        {"exclude_patterns", e.exclude_patterns},
    };
}

inline void from_json(const nlohmann::json& j, ScheduleEntry& e) {
    e.name = j.value("name", "");
    e.path = j.value("path", "");
    Interval iv = Interval::Daily;
    interval_from_string(j.value("interval", "daily"), iv);
    e.interval = iv;
    e.password = j.value("password", "");
    e.incremental = j.value("incremental", true);
    double ts = j.value("last_run", 0.0);
    e.last_run = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>(ts)));
    e.exclude_patterns = j.value("exclude_patterns", std::vector<std::string>{});
}

class ScheduleManager {
public:
    ScheduleManager() = default;

    bool create(const ScheduleEntry& entry);
    bool remove(const std::string& name);
    std::vector<ScheduleEntry> list() const;
    // Runs the snapshot now. Needs live vault; password falls back to
    // TELEVAULT_PASSWORD when the entry stores none.
    bool run(const std::string& name, TeleVault& vault, TelegramClient& tg);
    bool load(const std::string& name, ScheduleEntry& out) const;

    // System integration
    bool install_systemd_timer(const ScheduleEntry& entry);
    bool uninstall_systemd_timer(const std::string& name);
    std::string generate_systemd_unit(const ScheduleEntry& entry) const;
    std::string generate_cron_entry(const ScheduleEntry& entry) const;

private:
    std::string schedules_dir() const;
};

} // namespace tv
