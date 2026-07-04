#include "schedule.hpp"
#include "../util/config.hpp"
#include <spdlog/spdlog.h>

namespace tv {

class ScheduleManager::Impl {};

ScheduleManager::ScheduleManager() : impl_(std::make_unique<Impl>()) {}

std::string ScheduleManager::schedules_dir() const {
    return ConfigManager::instance().config_dir() + "/schedules";
}

bool ScheduleManager::create(const ScheduleEntry&) {
    spdlog::warn("ScheduleManager::create not yet implemented");
    return false;
}

bool ScheduleManager::remove(const std::string&) {
    spdlog::warn("ScheduleManager::remove not yet implemented");
    return false;
}

std::vector<ScheduleEntry> ScheduleManager::list() const {
    spdlog::warn("ScheduleManager::list not yet implemented");
    return {};
}

bool ScheduleManager::run(const std::string&) {
    spdlog::warn("ScheduleManager::run not yet implemented");
    return false;
}

bool ScheduleManager::install_systemd_timer(const ScheduleEntry&) {
    spdlog::warn("install_systemd_timer not yet implemented");
    return false;
}

bool ScheduleManager::uninstall_systemd_timer(const std::string&) {
    spdlog::warn("uninstall_systemd_timer not yet implemented");
    return false;
}

std::string ScheduleManager::generate_systemd_unit(const ScheduleEntry&) const {
    spdlog::warn("generate_systemd_unit not yet implemented");
    return {};
}

std::string ScheduleManager::generate_cron_entry(const ScheduleEntry&) const {
    spdlog::warn("generate_cron_entry not yet implemented");
    return {};
}

} // namespace tv
