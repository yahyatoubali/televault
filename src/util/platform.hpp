#pragma once

#include <string>
#include <optional>

namespace tv {

struct SystemInfo {
    long page_size{};
    long total_ram{};
    int cpu_count{};
    std::string os_name;
};

[[nodiscard]] SystemInfo get_system_info();
[[nodiscard]] bool is_low_resource_system();
[[nodiscard]] std::optional<std::string> detect_terminal();
[[nodiscard]] bool supports_kitty_protocol();
[[nodiscard]] std::string get_temp_dir();

} // namespace tv
