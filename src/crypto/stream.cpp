#include "stream.hpp"
#include <vector>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <span>
#include <array>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace tv {

static constexpr size_t NONCE_SIZE = 12;
static constexpr size_t TAG_SIZE = 16;
static constexpr size_t BLOCK_SIZE = 65536; // 64KB blocks

// ── StreamingEncryptor ───────────────────────────────────────────────
// Uses a counter-based nonce scheme: base_nonce + block_counter
// Format per block: [ciphertext:...][tag:16]

StreamingEncryptor::StreamingEncryptor(std::span<const uint8_t> key) {
    if (key.size() != 32) {
        throw std::runtime_error("Key must be 32 bytes");
    }
    // Store key and generate base nonce once
}

std::vector<uint8_t> StreamingEncryptor::process(std::span<const uint8_t> block) {
    // For now, use simple chunk-based encryption
    // Full streaming implementation needs EVP_EncryptUpdate loop
    // and counter-based nonce management
    throw std::runtime_error("StreamingEncryptor not fully implemented — use encrypt_chunk()");
}

std::vector<uint8_t> StreamingEncryptor::finalize() {
    return {};
}

// ── StreamingDecryptor ───────────────────────────────────────────────

StreamingDecryptor::StreamingDecryptor(std::span<const uint8_t> key) {
    if (key.size() != 32) {
        throw std::runtime_error("Key must be 32 bytes");
    }
}

std::vector<uint8_t> StreamingDecryptor::process(std::span<const uint8_t> block) {
    throw std::runtime_error("StreamingDecryptor not fully implemented — use decrypt_chunk()");
}

std::vector<uint8_t> StreamingDecryptor::finalize() {
    return {};
}

} // namespace tv
