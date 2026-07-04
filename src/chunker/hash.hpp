#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <span>

namespace tv {

[[nodiscard]] std::string hash_data(std::span<const uint8_t> data);
[[nodiscard]] std::string hash_file(const std::string& path);
[[nodiscard]] std::string hash_data_async(std::span<const uint8_t> data);
[[nodiscard]] std::string hash_file_async(const std::string& path);

} // namespace tv
