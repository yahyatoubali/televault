#pragma once

#include <string>
#include <memory>
#include <vector>
#include <chrono>
#include "../models/config.hpp"

namespace tv {

enum class Interval { Hourly, Daily, Weekly, Monthly };

struct ScheduleEntry {
    std::string name;
    std::string path;
    Interval interval{Interval::Daily};
    std::string password;
    bool incremental{true};
    std::chrono::system_clock::time_point last_run;
    std::vector<std::string> exclude_patterns;
};

class ScheduleManager {
public:
    ScheduleManager();

    bool create(const ScheduleEntry& entry);
    bool remove(const std::string& name);
    std::vector<ScheduleEntry> list() const;
    bool run(const std::string& name);

    // System integration
    bool install_systemd_timer(const ScheduleEntry& entry);
    bool uninstall_systemd_timer(const std::string& name);
    std::string generate_systemd_unit(const ScheduleEntry& entry) const;
    std::string generate_cron_entry(const ScheduleEntry& entry) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    std::string schedules_dir() const;
};

} // namespace tv
