#include "schedule.hpp"
#include "systemd.hpp"
#include "../util/config.hpp"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cctype>
#include <sys/stat.h>
#include <unistd.h>

#ifdef TV_BUILD_TDLIB
#include "../backup/engine.hpp"
#include "../core/vault.hpp"
#endif

namespace tv {

namespace fs = std::filesystem;

std::string ScheduleManager::schedules_dir() const {
    return ConfigManager::instance().config_dir() + "/schedules";
}

static bool write_file_private(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec) {
        spdlog::error("Cannot create directory {}: {}", p.parent_path().string(), ec.message());
        return false;
    }
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        spdlog::error("Cannot write file {}", p.string());
        return false;
    }
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    f.close();
    ::chmod(p.c_str(), 0600);
    return true;
}

static std::string sanitized(const std::string& name) {
    std::string out = name;
    for (auto& c : out) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    }
    if (out.empty()) out = "default";
    return out;
}

static std::string self_exe() {
    char buf[4096] = {};
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) return std::string(buf, static_cast<size_t>(n));
    return "televault";
}

bool ScheduleManager::create(const ScheduleEntry& entry) {
    if (entry.name.empty()) {
        spdlog::error("Schedule name cannot be empty");
        return false;
    }
    if (sanitized(entry.name) != entry.name) {
        spdlog::error("Schedule name may only contain letters, digits, '-' and '_'");
        return false;
    }
    if (entry.path.empty()) {
        spdlog::error("Schedule path cannot be empty");
        return false;
    }
    if (!fs::exists(entry.path)) {
        spdlog::warn("Scheduled path does not exist yet: {}", entry.path);
    }
    if (!entry.exclude_patterns.empty()) {
        spdlog::warn("Exclude patterns are stored but not yet applied by snapshot runs");
    }
    nlohmann::json j = entry;
    fs::path p = fs::path(schedules_dir()) / (entry.name + ".json");
    if (!write_file_private(p, j.dump(2))) return false;
    // Companion env file keeps the password out of unit files/ps output.
    if (!entry.password.empty()) {
        fs::path env = fs::path(schedules_dir()) / (entry.name + ".env");
        if (!write_file_private(env, "TELEVAULT_PASSWORD=" + entry.password + "\n")) return false;
    }
    spdlog::info("Schedule '{}' saved", entry.name);
    return true;
}

bool ScheduleManager::load(const std::string& name, ScheduleEntry& out) const {
    fs::path p = fs::path(schedules_dir()) / (name + ".json");
    std::ifstream f(p);
    if (!f) return false;
    try {
        nlohmann::json j;
        f >> j;
        out = j.get<ScheduleEntry>();
        return !out.name.empty();
    } catch (const std::exception& e) {
        spdlog::error("Cannot parse schedule '{}': {}", name, e.what());
        return false;
    }
}

std::vector<ScheduleEntry> ScheduleManager::list() const {
    std::vector<ScheduleEntry> out;
    std::error_code ec;
    if (!fs::exists(schedules_dir(), ec)) return out;
    for (auto& de : fs::directory_iterator(schedules_dir(), ec)) {
        if (ec) break;
        if (!de.is_regular_file()) continue;
        if (de.path().extension() != ".json") continue;
        std::ifstream f(de.path());
        if (!f) continue;
        try {
            nlohmann::json j;
            f >> j;
            ScheduleEntry e = j.get<ScheduleEntry>();
            if (!e.name.empty()) out.push_back(std::move(e));
        } catch (...) {
            spdlog::warn("Skipping unreadable schedule file {}", de.path().string());
        }
    }
    return out;
}

bool ScheduleManager::remove(const std::string& name) {
    uninstall_systemd_timer(name); // best effort
    std::error_code ec;
    bool ok = true;
    ok &= fs::remove(fs::path(schedules_dir()) / (name + ".json"), ec) || ec == std::errc::no_such_file_or_directory;
    ec.clear();
    fs::remove(fs::path(schedules_dir()) / (name + ".env"), ec); // optional companion
    if (ok) spdlog::info("Schedule '{}' removed", name);
    else spdlog::error("Cannot remove schedule '{}'", name);
    return ok;
}

bool ScheduleManager::run(const std::string& name, TeleVault& vault, TelegramClient& tg) {
#ifdef TV_BUILD_TDLIB
    ScheduleEntry e;
    if (!load(name, e)) {
        spdlog::error("Schedule not found: {}", name);
        return false;
    }
    std::string pw = e.password;
    if (pw.empty()) {
        if (const char* env = std::getenv("TELEVAULT_PASSWORD")) pw = env;
    }
    if (pw.empty()) {
        spdlog::error("Schedule '{}' has no password (set one or export TELEVAULT_PASSWORD)", name);
        return false;
    }
    if (!fs::exists(e.path)) {
        spdlog::error("Scheduled path does not exist: {}", e.path);
        return false;
    }
    BackupEngine engine(vault, tg);
    std::string snap_name = e.name + "_" + interval_to_string(e.interval);
    spdlog::info("Running schedule '{}' -> snapshot '{}'", name, snap_name);
    bool ok = engine.create_snapshot(snap_name, {e.path}, pw, e.incremental);
    if (ok) {
        e.last_run = std::chrono::system_clock::now();
        ScheduleEntry cur;
        if (load(name, cur)) {
            cur.last_run = e.last_run;
            nlohmann::json j = cur;
            write_file_private(fs::path(schedules_dir()) / (name + ".json"), j.dump(2));
        }
    }
    return ok;
#else
    (void)name;
    (void)vault;
    (void)tg;
    spdlog::error("Scheduled runs need a Telegram-enabled build (-DTV_BUILD_TDLIB=ON)");
    return false;
#endif
}

std::string ScheduleManager::generate_systemd_unit(const ScheduleEntry& entry) const {
    return generate_service_unit(entry.name, self_exe() + " backup create \"" +
        entry.path + "\" -n \"" + entry.name + "\"" +
        (entry.incremental ? " --incremental" : ""));
}

std::string ScheduleManager::generate_cron_entry(const ScheduleEntry& entry) const {
    std::string slot = "@daily";
    switch (entry.interval) {
        case Interval::Hourly: slot = "@hourly"; break;
        case Interval::Daily: slot = "@daily"; break;
        case Interval::Weekly: slot = "@weekly"; break;
        case Interval::Monthly: slot = "@monthly"; break;
    }
    return slot + " " + self_exe() + " backup create \"" + entry.path +
        "\" -n \"" + entry.name + "\"" + (entry.incremental ? " --incremental" : "");
}

bool ScheduleManager::install_systemd_timer(const ScheduleEntry& entry) {
    std::string cal;
    switch (entry.interval) {
        case Interval::Hourly: cal = "hourly"; break;
        case Interval::Daily: cal = "daily"; break;
        case Interval::Weekly: cal = "weekly"; break;
        case Interval::Monthly: cal = "monthly"; break;
    }
    std::string timer = generate_timer_unit(entry.name, cal);
    // Service runs with the companion env file when a password is stored.
    fs::path env = fs::path(schedules_dir()) / (entry.name + ".env");
    std::string cmd = self_exe() + " backup create \"" + entry.path +
        "\" -n \"" + entry.name + "\"" + (entry.incremental ? " --incremental" : "");
    std::string service = generate_service_unit(entry.name, cmd);
    if (fs::exists(env)) {
        // Must live under [Service], i.e. right after ExecStart=.
        const std::string anchor = "ExecStart=";
        auto pos = service.find(anchor);
        if (pos != std::string::npos) {
            auto eol = service.find('\n', pos);
            service.insert(eol + 1, "EnvironmentFile=" + env.string() + "\n");
        }
    }
    std::string unit_name = sanitized(entry.name);
    if (!install_unit(unit_name, service, "service")) return false;
    if (!install_unit(unit_name, timer, "timer")) return false;
    if (!enable_timer(unit_name)) {
        spdlog::warn("Timer files written but systemctl enable failed; start manually with "
                     "'systemctl --user enable --now televault-sched-{}.timer'", unit_name);
        return false;
    }
    spdlog::info("Systemd timer 'televault-sched-{}' installed and started", unit_name);
    return true;
}

bool ScheduleManager::uninstall_systemd_timer(const std::string& name) {
    return uninstall_unit("televault-sched-" + sanitized(name), "timer") &&
           uninstall_unit("televault-sched-" + sanitized(name), "service");
}

} // namespace tv
