#include "stream.hpp"
#include <vector>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <span>
#include <array>
#include <memory>
#include <limits>
#include <algorithm>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace tv {

static constexpr size_t NONCE_SIZE = 12;
static constexpr size_t TAG_SIZE = 16;
static constexpr size_t KEY_SIZE = 32;

struct CipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* ctx) const noexcept {
        if (ctx) {
            EVP_CIPHER_CTX_free(ctx);
        }
    }
};
using UniqueCipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter>;

static std::array<uint8_t, NONCE_SIZE> derive_counter_nonce(
    const std::array<uint8_t, NONCE_SIZE>& base_nonce,
    uint32_t counter
) {
    std::array<uint8_t, NONCE_SIZE> nonce{};
    std::copy_n(base_nonce.begin(), 8, nonce.begin());
    nonce[8]  = static_cast<uint8_t>((counter >> 24) & 0xFF);
    nonce[9]  = static_cast<uint8_t>((counter >> 16) & 0xFF);
    nonce[10] = static_cast<uint8_t>((counter >> 8)  & 0xFF);
    nonce[11] = static_cast<uint8_t>(counter         & 0xFF);
    return nonce;
}

// ── StreamingEncryptor ───────────────────────────────────────────────

StreamingEncryptor::StreamingEncryptor(std::span<const uint8_t> key) {
    if (key.size() != KEY_SIZE) {
        throw std::runtime_error("Key must be 32 bytes for StreamingEncryptor");
    }
    std::copy_n(key.data(), KEY_SIZE, key_.begin());
    if (RAND_bytes(base_nonce_.data(), static_cast<int>(NONCE_SIZE)) != 1) {
        throw std::runtime_error("Failed to generate random base nonce");
    }
}

StreamingEncryptor::StreamingEncryptor(std::span<const uint8_t> key, std::span<const uint8_t> base_nonce) {
    if (key.size() != KEY_SIZE) {
        throw std::runtime_error("Key must be 32 bytes for StreamingEncryptor");
    }
    if (base_nonce.size() != NONCE_SIZE) {
        throw std::runtime_error("Base nonce must be 12 bytes");
    }
    std::copy_n(key.data(), KEY_SIZE, key_.begin());
    std::copy_n(base_nonce.data(), NONCE_SIZE, base_nonce_.begin());
}

void StreamingEncryptor::set_base_nonce(std::span<const uint8_t> base_nonce) {
    if (base_nonce.size() != NONCE_SIZE) {
        throw std::runtime_error("Base nonce must be 12 bytes");
    }
    std::copy_n(base_nonce.data(), NONCE_SIZE, base_nonce_.begin());
}

std::vector<uint8_t> StreamingEncryptor::process(std::span<const uint8_t> block) {
    if (block.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Block size exceeds maximum supported length");
    }

    auto nonce = derive_counter_nonce(base_nonce_, counter_++);

    UniqueCipherCtx ctx(EVP_CIPHER_CTX_new());
    if (!ctx) throw std::runtime_error("Failed to create cipher context");

    std::vector<uint8_t> ct(block.size() + 16);
    int len = 0, ct_len = 0;

    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        throw std::runtime_error("Failed to init cipher");
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(NONCE_SIZE), nullptr) != 1) {
        throw std::runtime_error("Failed to set IV length");
    }
    if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key_.data(), nonce.data()) != 1) {
        throw std::runtime_error("Failed to init key/nonce");
    }

    if (!block.empty()) {
        if (EVP_EncryptUpdate(ctx.get(), ct.data(), &len, block.data(), static_cast<int>(block.size())) != 1) {
            throw std::runtime_error("Streaming encryption update failed");
        }
        ct_len = len;
    }

    if (EVP_EncryptFinal_ex(ctx.get(), ct.data() + ct_len, &len) != 1) {
        throw std::runtime_error("Streaming encryption finalization failed");
    }
    ct_len += len;
    ct.resize(ct_len);

    std::array<uint8_t, TAG_SIZE> tag{};
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(TAG_SIZE), tag.data()) != 1) {
        throw std::runtime_error("Failed to get auth tag");
    }

    std::vector<uint8_t> result;
    result.reserve(ct.size() + TAG_SIZE);
    result.insert(result.end(), ct.begin(), ct.end());
    result.insert(result.end(), tag.begin(), tag.end());
    return result;
}

std::vector<uint8_t> StreamingEncryptor::finalize() {
    return {};
}

// ── StreamingDecryptor ───────────────────────────────────────────────

StreamingDecryptor::StreamingDecryptor(std::span<const uint8_t> key) {
    if (key.size() != KEY_SIZE) {
        throw std::runtime_error("Key must be 32 bytes for StreamingDecryptor");
    }
    std::copy_n(key.data(), KEY_SIZE, key_.begin());
}

StreamingDecryptor::StreamingDecryptor(std::span<const uint8_t> key, std::span<const uint8_t> base_nonce) {
    if (key.size() != KEY_SIZE) {
        throw std::runtime_error("Key must be 32 bytes for StreamingDecryptor");
    }
    if (base_nonce.size() != NONCE_SIZE) {
        throw std::runtime_error("Base nonce must be 12 bytes");
    }
    std::copy_n(key.data(), KEY_SIZE, key_.begin());
    std::copy_n(base_nonce.data(), NONCE_SIZE, base_nonce_.begin());
}

void StreamingDecryptor::set_base_nonce(std::span<const uint8_t> base_nonce) {
    if (base_nonce.size() != NONCE_SIZE) {
        throw std::runtime_error("Base nonce must be 12 bytes");
    }
    std::copy_n(base_nonce.data(), NONCE_SIZE, base_nonce_.begin());
}

std::vector<uint8_t> StreamingDecryptor::process(std::span<const uint8_t> block) {
    if (block.size() < TAG_SIZE) {
        throw std::runtime_error("Encrypted block too short — must include 16-byte tag");
    }
    if (block.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Block size exceeds maximum supported length");
    }

    auto nonce = derive_counter_nonce(base_nonce_, counter_++);
    size_t ct_len_val = block.size() - TAG_SIZE;
    auto ct = block.subspan(0, ct_len_val);
    auto tag = block.subspan(ct_len_val, TAG_SIZE);

    UniqueCipherCtx ctx(EVP_CIPHER_CTX_new());
    if (!ctx) throw std::runtime_error("Failed to create cipher context");

    std::vector<uint8_t> pt(ct.size());
    int len = 0, pt_len = 0;

    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        throw std::runtime_error("Failed to init cipher");
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(NONCE_SIZE), nullptr) != 1) {
        throw std::runtime_error("Failed to set IV length");
    }
    if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key_.data(), nonce.data()) != 1) {
        throw std::runtime_error("Failed to init key/nonce");
    }

    if (!ct.empty()) {
        if (EVP_DecryptUpdate(ctx.get(), pt.data(), &len, ct.data(), static_cast<int>(ct.size())) != 1) {
            throw std::runtime_error("Streaming decryption update failed");
        }
        pt_len = len;
    }

    std::array<uint8_t, TAG_SIZE> tag_buf{};
    std::copy_n(tag.data(), TAG_SIZE, tag_buf.begin());
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(TAG_SIZE), tag_buf.data()) != 1) {
        throw std::runtime_error("Failed to set auth tag");
    }

    int ret = EVP_DecryptFinal_ex(ctx.get(), pt.data() + pt_len, &len);
    if (ret <= 0) {
        throw std::runtime_error("Streaming decryption failed — authentication tag mismatch");
    }
    pt_len += len;
    pt.resize(pt_len);
    return pt;
}

std::vector<uint8_t> StreamingDecryptor::finalize() {
    return {};
}

} // namespace tv
