#pragma once

#include <vector>
#include <cstdint>
#include <span>
#include <string_view>
#include <filesystem>

namespace tv {

inline constexpr int DEFAULT_COMPRESSION_LEVEL = 3;

[[nodiscard]] bool should_compress(std::string_view filename);

[[nodiscard]] inline bool is_compressible(std::string_view filename) {
    return should_compress(filename);
}

[[nodiscard]] std::vector<uint8_t> compress_data(std::span<const uint8_t> data,
                                                 int level = DEFAULT_COMPRESSION_LEVEL);

[[nodiscard]] std::vector<uint8_t> decompress_data(std::span<const uint8_t> data);

// Returns true if data starts with a valid zstd frame magic.
[[nodiscard]] bool is_zstd_frame(std::span<const uint8_t> data) noexcept;

// Tolerant decompress for vault reads: if compressed_flag is false (or data
// is empty) returns data as-is. If flag is true but bytes are not a zstd
// frame (legacy files pushed with the mp4-bypass flag mismatch), logs a
// warning and returns data as-is instead of throwing.
[[nodiscard]] std::vector<uint8_t> decompress_data_tolerant(std::span<const uint8_t> data,
                                                            bool compressed_flag);

[[nodiscard]] uint64_t estimate_compressed_size(uint64_t original_size,
                                                std::string_view filename = "");

[[nodiscard]] uint64_t compress_bound(uint64_t uncompressed_size);

[[nodiscard]] double compress_file(const std::filesystem::path& input_path,
                                  const std::filesystem::path& output_path,
                                  int level = DEFAULT_COMPRESSION_LEVEL);

void decompress_file(const std::filesystem::path& input_path,
                     const std::filesystem::path& output_path);

} // namespace tv
