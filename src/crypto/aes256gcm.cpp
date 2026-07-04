#include "aes256gcm.hpp"
#include <vector>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <span>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace tv {

static constexpr size_t NONCE_SIZE = 12;
static constexpr size_t TAG_SIZE = 16;
static constexpr size_t KEY_SIZE = 32;

std::vector<uint8_t> encrypt_chunk(std::span<const uint8_t> data, std::span<const uint8_t> key) {
    if (key.size() != KEY_SIZE) {
        throw std::runtime_error("Key must be 32 bytes for AES-256");
    }

    // Generate random nonce
    std::vector<uint8_t> nonce(NONCE_SIZE);
    if (RAND_bytes(nonce.data(), NONCE_SIZE) != 1) {
        throw std::runtime_error("Failed to generate random nonce");
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("Failed to create cipher context");

    // Encrypt
    std::vector<uint8_t> ct(data.size() + 16); // extra room for GCM overhead
    int len = 0, ct_len = 0;

    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, NONCE_SIZE, nullptr);
    EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data());

    EVP_EncryptUpdate(ctx, ct.data(), &len, data.data(), static_cast<int>(data.size()));
    ct_len = len;

    EVP_EncryptFinal_ex(ctx, ct.data() + ct_len, &len);
    ct_len += len;
    ct.resize(ct_len);

    // Get authentication tag
    std::vector<uint8_t> tag(TAG_SIZE);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_SIZE, tag.data());
    EVP_CIPHER_CTX_free(ctx);

    // Format: [nonce:12][ciphertext:...][tag:16]
    std::vector<uint8_t> result;
    result.reserve(NONCE_SIZE + ct.size() + TAG_SIZE);
    result.insert(result.end(), nonce.begin(), nonce.end());
    result.insert(result.end(), ct.begin(), ct.end());
    result.insert(result.end(), tag.begin(), tag.end());

    return result;
}

std::vector<uint8_t> decrypt_chunk(std::span<const uint8_t> ciphertext, std::span<const uint8_t> key) {
    if (key.size() != KEY_SIZE) {
        throw std::runtime_error("Key must be 32 bytes for AES-256");
    }
    if (ciphertext.size() < NONCE_SIZE + TAG_SIZE) {
        throw std::runtime_error("Ciphertext too short");
    }

    auto nonce = ciphertext.subspan(0, NONCE_SIZE);
    auto ct = ciphertext.subspan(NONCE_SIZE, ciphertext.size() - NONCE_SIZE - TAG_SIZE);
    auto tag = ciphertext.subspan(ciphertext.size() - TAG_SIZE, TAG_SIZE);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("Failed to create cipher context");

    std::vector<uint8_t> pt(ct.size());
    int len = 0, pt_len = 0;

    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, NONCE_SIZE, nullptr);
    EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data());

    EVP_DecryptUpdate(ctx, pt.data(), &len, ct.data(), static_cast<int>(ct.size()));
    pt_len = len;

    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_SIZE,
                        const_cast<uint8_t*>(tag.data()));

    int ret = EVP_DecryptFinal_ex(ctx, pt.data() + pt_len, &len);
    EVP_CIPHER_CTX_free(ctx);

    if (ret <= 0) {
        throw std::runtime_error("Decryption failed — authentication tag mismatch");
    }

    pt_len += len;
    pt.resize(pt_len);
    return pt;
}

} // namespace tv
