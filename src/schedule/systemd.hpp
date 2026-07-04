#pragma once

#include <string>

namespace tv {

std::string generate_timer_unit(const std::string& name, const std::string& interval);
std::string generate_service_unit(const std::string& name, const std::string& command);
bool install_unit(const std::string& name, const std::string& content, const std::string& suffix);
bool uninstall_unit(const std::string& name, const std::string& suffix);
bool enable_timer(const std::string& name);

} // namespace tv
