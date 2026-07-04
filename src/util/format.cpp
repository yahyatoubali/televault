#include "format.hpp"
#include <format>

namespace tv {

std::string format_size(uint64_t bytes) {
    static constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int unit_idx = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_idx < 5) {
        size /= 1024.0;
        ++unit_idx;
    }

    if (unit_idx == 0) {
        return std::format("{} {}", bytes, units[0]);
    }
    return std::format("{:.2f} {}", size, units[unit_idx]);
}

std::string format_speed(double bytes_per_sec) {
    static constexpr const char* units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int unit_idx = 0;
    double speed = bytes_per_sec;

    while (speed >= 1024.0 && unit_idx < 3) {
        speed /= 1024.0;
        ++unit_idx;
    }

    return std::format("{:.2f} {}", speed, units[unit_idx]);
}

} // namespace tv
