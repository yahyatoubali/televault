#include "platform.hpp"
#include <unistd.h>
#include <sys/sysinfo.h>
#include <cstdlib>

namespace tv {

SystemInfo get_system_info() {
    SystemInfo info{};
    info.page_size = sysconf(_SC_PAGESIZE);
    info.total_ram = sysconf(_SC_PHYS_PAGES) * info.page_size;
    info.cpu_count = sysconf(_SC_NPROCESSORS_CONF);
    info.os_name = "Linux";
    return info;
}

bool is_low_resource_system() {
    auto info = get_system_info();
    return info.total_ram < 2LL * 1024 * 1024 * 1024 || info.cpu_count <= 2;
}

std::optional<std::string> detect_terminal() {
    if (auto* term = std::getenv("TERM")) return std::string(term);
    if (auto* term_prog = std::getenv("TERM_PROGRAM")) return std::string(term_prog);
    return std::nullopt;
}

bool supports_kitty_protocol() {
    if (auto* term = std::getenv("TERM")) {
        return std::string_view(term).find("kitty") != std::string_view::npos;
    }
    return false;
}

std::string get_temp_dir() {
    if (auto* tmp = std::getenv("TMPDIR")) return tmp;
    if (auto* tmp = std::getenv("TMP")) return tmp;
    return "/tmp";
}

} // namespace tv
