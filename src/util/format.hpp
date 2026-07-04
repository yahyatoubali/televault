#pragma once

#include <string>
#include <cstdint>

namespace tv {

[[nodiscard]] std::string format_size(uint64_t bytes);
[[nodiscard]] std::string format_speed(double bytes_per_sec);

} // namespace tv
