#pragma once

#include <vector>
#include <cstdint>
#include <span>

namespace tv {

struct EncryptionHeader {
    std::array<uint8_t, 16> salt{};
    std::array<uint8_t, 12> nonce{};
};

std::vector<uint8_t> encrypt_chunk(std::span<const uint8_t> data, std::span<const uint8_t> key);
std::vector<uint8_t> decrypt_chunk(std::span<const uint8_t> ciphertext, std::span<const uint8_t> key);

} // namespace tv
