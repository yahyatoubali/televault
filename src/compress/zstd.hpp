#pragma once

#include <vector>
#include <cstdint>
#include <span>
#include <string_view>

namespace tv {

[[nodiscard]] bool should_compress(std::string_view filename);
[[nodiscard]] std::vector<uint8_t> compress_data(std::span<const uint8_t> data, int level = 3);
[[nodiscard]] std::vector<uint8_t> decompress_data(std::span<const uint8_t> data);
[[nodiscard]] uint64_t estimate_compressed_size(uint64_t uncompressed_size);

} // namespace tv
