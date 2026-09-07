#include "platform.hpp"
#include <unistd.h>
#include <cstdlib>

#if defined(__APPLE__) && defined(__MACH__)
#include <sys/types.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#endif

namespace tv {

SystemInfo get_system_info() {
    SystemInfo info{};
    info.page_size = sysconf(_SC_PAGESIZE);

#if defined(__APPLE__) && defined(__MACH__)
    int64_t mem = 0;
    size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) {
        info.total_ram = mem;
    } else {
        info.total_ram = sysconf(_SC_PHYS_PAGES) * info.page_size;
    }
    info.cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    info.os_name = "macOS";
#elif defined(__linux__)
    info.total_ram = sysconf(_SC_PHYS_PAGES) * info.page_size;
    info.cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    info.os_name = "Linux";
#elif defined(_WIN32)
    info.total_ram = sysconf(_SC_PHYS_PAGES) * info.page_size;
    info.cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    info.os_name = "Windows";
#else
    info.total_ram = sysconf(_SC_PHYS_PAGES) * info.page_size;
    info.cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    info.os_name = "Unknown";
#endif

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
