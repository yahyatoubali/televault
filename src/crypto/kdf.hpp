#pragma once

#include <vector>
#include <cstdint>
#include <array>
#include <string>
#include <span>
#include <string_view>

namespace tv {

std::array<uint8_t, 32> derive_key(std::string_view password, std::span<const uint8_t> salt);

} // namespace tv
