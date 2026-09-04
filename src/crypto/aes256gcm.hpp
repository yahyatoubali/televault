#pragma once

#include <vector>
#include <cstdint>
#include <span>
#include <array>
#include <string>
#include <string_view>

namespace tv {

// Standard TeleVault Cryptographic Constants
inline constexpr size_t SALT_SIZE = 16;
inline constexpr size_t NONCE_SIZE = 12;
inline constexpr size_t TAG_SIZE = 16;
inline constexpr size_t KEY_SIZE = 32;
inline constexpr size_t HEADER_SIZE = SALT_SIZE + NONCE_SIZE; // 28 bytes
inline constexpr size_t WIRE_OVERHEAD = HEADER_SIZE + TAG_SIZE; // 44 bytes
inline constexpr size_t LEGACY_OVERHEAD = NONCE_SIZE + TAG_SIZE; // 28 bytes

struct EncryptionHeader {
    std::array<uint8_t, SALT_SIZE> salt{};
    std::array<uint8_t, NONCE_SIZE> nonce{};

    static constexpr size_t SIZE = HEADER_SIZE;

    [[nodiscard]] std::vector<uint8_t> to_bytes() const;
    [[nodiscard]] static EncryptionHeader from_bytes(std::span<const uint8_t> data);
    [[nodiscard]] static EncryptionHeader generate();
};

// Key-based chunk encryption/decryption (primary wire format: [salt:16][nonce:12][ciphertext][tag:16])
std::vector<uint8_t> encrypt_chunk(
    std::span<const uint8_t> data,
    std::span<const uint8_t> key,
    std::span<const uint8_t> salt = {}
);

// Dual-format chunk decryption: transparently handles 44-byte format (with salt)
// and legacy 28-byte format (without salt, for backward compatibility)
std::vector<uint8_t> decrypt_chunk(
    std::span<const uint8_t> ciphertext,
    std::span<const uint8_t> key
);

// Password-based chunk encryption/decryption matching Python reference
std::vector<uint8_t> encrypt_chunk(
    std::span<const uint8_t> data,
    std::string_view password,
    std::span<const uint8_t> salt = {}
);

std::vector<uint8_t> decrypt_chunk(
    std::span<const uint8_t> ciphertext,
    std::string_view password,
    std::span<const uint8_t> fallback_salt = {}
);

// Whole-file encryption/decryption utilities
void encrypt_file_simple(
    const std::string& input_path,
    const std::string& output_path,
    std::string_view password
);

void decrypt_file_simple(
    const std::string& input_path,
    const std::string& output_path,
    std::string_view password
);

} // namespace tv
