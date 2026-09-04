#pragma once

#include <vector>
#include <cstdint>
#include <array>
#include <string>
#include <span>
#include <string_view>

namespace tv {

// Scrypt key derivation (matches Python src/televault/crypto.py)
std::array<uint8_t, 32> derive_key_scrypt(
    std::string_view password,
    std::span<const uint8_t> salt,
    uint64_t n = 1 << 17,
    uint32_t r = 8,
    uint32_t p = 1
);

// PBKDF2-HMAC-SHA256 key derivation (matches CryptoOracle / test harness)
std::array<uint8_t, 32> derive_key_pbkdf2(
    std::string_view password,
    std::span<const uint8_t> salt,
    uint32_t iterations = 100000
);

// Argon2id key derivation via OpenSSL 3.2+ EVP_KDF
std::array<uint8_t, 32> derive_key_argon2id(
    std::string_view password,
    std::span<const uint8_t> salt,
    uint32_t iterations = 3,
    uint32_t memory_kib = 65536,
    uint32_t parallelism = 1
);

// Default key derivation (forwards to Scrypt for backward compatibility)
std::array<uint8_t, 32> derive_key(
    std::string_view password,
    std::span<const uint8_t> salt
);

} // namespace tv
