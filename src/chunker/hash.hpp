#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <span>
#include <string_view>
#include <blake3.h>

namespace tv {

static constexpr size_t BLAKE3_FULL_HEX_SIZE = 64;   // 32 bytes -> 64 hex characters
static constexpr size_t BLAKE3_PREFIX_HEX_SIZE = 32; // 16 bytes -> 32 hex characters

[[nodiscard]] std::string hash_to_hex(std::span<const uint8_t> hash, size_t prefix_len = BLAKE3_FULL_HEX_SIZE);
[[nodiscard]] std::string hash_data(std::span<const uint8_t> data, size_t prefix_len = BLAKE3_FULL_HEX_SIZE);
[[nodiscard]] std::string hash_file(const std::string& path, size_t prefix_len = BLAKE3_FULL_HEX_SIZE);
[[nodiscard]] std::string hash_data_prefix(std::span<const uint8_t> data, size_t prefix_len = BLAKE3_PREFIX_HEX_SIZE);
[[nodiscard]] std::string hash_file_prefix(const std::string& path, size_t prefix_len = BLAKE3_PREFIX_HEX_SIZE);
[[nodiscard]] std::string hash_data_async(std::span<const uint8_t> data, size_t prefix_len = BLAKE3_FULL_HEX_SIZE);
[[nodiscard]] std::string hash_file_async(const std::string& path, size_t prefix_len = BLAKE3_FULL_HEX_SIZE);

// Incremental multi-part BLAKE3 hasher
class Blake3Hasher {
public:
    Blake3Hasher();
    void update(std::span<const uint8_t> data);
    void update(const void* data, size_t size);
    [[nodiscard]] std::string finalize(size_t prefix_len = BLAKE3_FULL_HEX_SIZE);
    void reset();

private:
    blake3_hasher hasher_;
};

// Flexible hash comparison: matches if either hash is a prefix of the other, or exact match
[[nodiscard]] bool hash_matches(std::string_view computed, std::string_view expected) noexcept;

} // namespace tv
