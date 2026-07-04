#include "systemd.hpp"
#include <spdlog/spdlog.h>

namespace tv {

std::string generate_timer_unit(const std::string&, const std::string&) {
    spdlog::warn("generate_timer_unit not yet implemented");
    return {};
}

std::string generate_service_unit(const std::string&, const std::string&) {
    spdlog::warn("generate_service_unit not yet implemented");
    return {};
}

bool install_unit(const std::string&, const std::string&, const std::string&) {
    spdlog::warn("install_unit not yet implemented");
    return false;
}

bool uninstall_unit(const std::string&, const std::string&) {
    spdlog::warn("uninstall_unit not yet implemented");
    return false;
}

bool enable_timer(const std::string&) {
    spdlog::warn("enable_timer not yet implemented");
    return false;
}

} // namespace tv
