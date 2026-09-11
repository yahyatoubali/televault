#include "systemd.hpp"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <array>

namespace tv {

namespace fs = std::filesystem;

static std::string sanitized(const std::string& name) {
    std::string out = name;
    for (auto& c : out) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    }
    return out.empty() ? std::string("default") : out;
}

static fs::path user_systemd_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path base = (xdg && *xdg) ? fs::path(xdg) : fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") / ".config";
    return base / "systemd" / "user";
}

static bool have_systemctl() {
    return std::system("command -v systemctl >/dev/null 2>&1") == 0;
}

static std::string run_capture(const std::string& cmd) {
    std::string out;
    std::array<char, 256> buf{};
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return out;
    while (std::fgets(buf.data(), buf.size(), p)) out += buf.data();
    ::pclose(p);
    return out;
}

std::string generate_timer_unit(const std::string& name, const std::string& interval) {
    std::string cal = "daily";
    std::string v = interval;
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "hourly" || v == "hour") cal = "hourly";
    else if (v == "daily" || v == "day") cal = "daily";
    else if (v == "weekly" || v == "week") cal = "weekly";
    else if (v == "monthly" || v == "month") cal = "monthly";

    std::string unit = sanitized(name);
    return "[Unit]\n"
           "Description=TeleVault scheduled backup '" + unit + "'\n"
           "After=network-online.target\n"
           "[Timer]\n"
           "OnCalendar=" + cal + "\n"
           "Persistent=true\n"
           "Unit=televault-sched-" + unit + ".service\n"
           "[Install]\n"
           "WantedBy=timers.target\n";
}

std::string generate_service_unit(const std::string& name, const std::string& command) {
    std::string unit = sanitized(name);
    return "[Unit]\n"
           "Description=TeleVault scheduled backup '" + unit + "'\n"
           "After=network-online.target\n"
           "[Service]\n"
           "Type=oneshot\n"
           "ExecStart=" + command + "\n"
           "[Install]\n"
           "WantedBy=multi-user.target\n";
}

bool install_unit(const std::string& name, const std::string& content, const std::string& suffix) {
    std::string unit = "televault-sched-" + sanitized(name);
    fs::path dir = user_systemd_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        spdlog::error("Cannot create {}: {}", dir.string(), ec.message());
        return false;
    }
    fs::path p = dir / (unit + "." + suffix);
    std::ofstream f(p, std::ios::trunc);
    if (!f) {
        spdlog::error("Cannot write {}", p.string());
        return false;
    }
    f << content;
    f.close();
    spdlog::info("Wrote {}", p.string());
    return true;
}

bool uninstall_unit(const std::string& name, const std::string& suffix) {
    std::string base = sanitized(name);
    if (base.rfind("televault-sched-", 0) != 0) base = "televault-sched-" + base;
    if (have_systemctl()) {
        std::string unit = base + "." + suffix;
        std::system(("systemctl --user disable --now " + unit + " >/dev/null 2>&1").c_str());
    }
    std::error_code ec;
    bool gone = fs::remove(user_systemd_dir() / (base + "." + suffix), ec);
    return gone || ec == std::errc::no_such_file_or_directory;
}

bool enable_timer(const std::string& name) {
    if (!have_systemctl()) {
        spdlog::warn("systemctl not found; cannot enable timer");
        return false;
    }
    std::string base = sanitized(name);
    if (base.rfind("televault-sched-", 0) != 0) base = "televault-sched-" + base;
    if (std::system("systemctl --user daemon-reload >/dev/null 2>&1") != 0) return false;
    std::string out = run_capture("systemctl --user enable --now " + base + ".timer 2>&1");
    if (out.find("Failed") != std::string::npos) {
        spdlog::error("systemctl enable failed: {}", out);
        return false;
    }
    return true;
}

} // namespace tv
