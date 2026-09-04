#include "aes256gcm.hpp"
#include "kdf.hpp"
#include <vector>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <span>
#include <memory>
#include <limits>
#include <fstream>
#include <algorithm>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace tv {

struct CipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* ctx) const noexcept {
        if (ctx) {
            EVP_CIPHER_CTX_free(ctx);
        }
    }
};
using UniqueCipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter>;

// ── EncryptionHeader Methods ──────────────────────────────────────────

std::vector<uint8_t> EncryptionHeader::to_bytes() const {
    std::vector<uint8_t> out;
    out.reserve(SIZE);
    out.insert(out.end(), salt.begin(), salt.end());
    out.insert(out.end(), nonce.begin(), nonce.end());
    return out;
}

EncryptionHeader EncryptionHeader::from_bytes(std::span<const uint8_t> data) {
    if (data.size() < SIZE) {
        throw std::runtime_error("Header too short: " + std::to_string(data.size()) + " < 28 bytes");
    }
    EncryptionHeader h;
    std::copy_n(data.data(), SALT_SIZE, h.salt.begin());
    std::copy_n(data.data() + SALT_SIZE, NONCE_SIZE, h.nonce.begin());
    return h;
}

EncryptionHeader EncryptionHeader::generate() {
    EncryptionHeader h;
    if (RAND_bytes(h.salt.data(), static_cast<int>(SALT_SIZE)) != 1 ||
        RAND_bytes(h.nonce.data(), static_cast<int>(NONCE_SIZE)) != 1) {
        throw std::runtime_error("Failed to generate random salt/nonce");
    }
    return h;
}

// ── Internal Helper: Single-Block AES-256-GCM Encrypt ────────────────

static bool encrypt_gcm_block(
    std::span<const uint8_t> pt,
    std::span<const uint8_t> key,
    std::span<const uint8_t> nonce,
    std::vector<uint8_t>& ct,
    std::array<uint8_t, TAG_SIZE>& tag
) {
    if (key.size() != KEY_SIZE || nonce.size() != NONCE_SIZE) {
        return false;
    }
    if (pt.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Plaintext size exceeds maximum supported block length");
    }

    UniqueCipherCtx ctx(EVP_CIPHER_CTX_new());
    if (!ctx) throw std::runtime_error("Failed to create cipher context");

    ct.resize(pt.size() + 16);
    int len = 0, ct_len = 0;

    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        throw std::runtime_error("Failed to init cipher");
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(NONCE_SIZE), nullptr) != 1) {
        throw std::runtime_error("Failed to set IV length");
    }
    if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) {
        throw std::runtime_error("Failed to init key/nonce");
    }

    if (!pt.empty()) {
        if (EVP_EncryptUpdate(ctx.get(), ct.data(), &len, pt.data(), static_cast<int>(pt.size())) != 1) {
            throw std::runtime_error("Encryption update failed");
        }
        ct_len = len;
    }

    if (EVP_EncryptFinal_ex(ctx.get(), ct.data() + ct_len, &len) != 1) {
        throw std::runtime_error("Encryption finalization failed");
    }
    ct_len += len;
    ct.resize(ct_len);

    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(TAG_SIZE), tag.data()) != 1) {
        throw std::runtime_error("Failed to get auth tag");
    }
    return true;
}

// ── Internal Helper: Single-Block AES-256-GCM Decrypt ────────────────

static bool decrypt_gcm_block(
    std::span<const uint8_t> ct,
    std::span<const uint8_t> tag,
    std::span<const uint8_t> key,
    std::span<const uint8_t> nonce,
    std::vector<uint8_t>& plaintext
) {
    if (key.size() != KEY_SIZE || nonce.size() != NONCE_SIZE || tag.size() != TAG_SIZE) {
        return false;
    }
    if (ct.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    UniqueCipherCtx ctx(EVP_CIPHER_CTX_new());
    if (!ctx) return false;

    plaintext.resize(ct.size());
    int len = 0, pt_len = 0;

    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) return false;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(NONCE_SIZE), nullptr) != 1) return false;
    if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) return false;

    if (!ct.empty()) {
        if (EVP_DecryptUpdate(ctx.get(), plaintext.data(), &len, ct.data(), static_cast<int>(ct.size())) != 1) {
            return false;
        }
        pt_len = len;
    }

    // Copy tag to mutable array to satisfy OpenSSL API
    std::array<uint8_t, TAG_SIZE> tag_buf{};
    std::copy_n(tag.data(), TAG_SIZE, tag_buf.begin());
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(TAG_SIZE), tag_buf.data()) != 1) {
        return false;
    }

    int ret = EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + pt_len, &len);
    if (ret <= 0) {
        return false;
    }
    pt_len += len;
    plaintext.resize(pt_len);
    return true;
}

// ── Chunk Encryption ──────────────────────────────────────────────────

std::vector<uint8_t> encrypt_chunk(
    std::span<const uint8_t> data,
    std::span<const uint8_t> key,
    std::span<const uint8_t> salt
) {
    if (key.size() != KEY_SIZE) {
        throw std::runtime_error("Key must be 32 bytes for AES-256");
    }

    std::array<uint8_t, SALT_SIZE> salt_buf{};
    if (salt.empty()) {
        if (RAND_bytes(salt_buf.data(), static_cast<int>(SALT_SIZE)) != 1) {
            throw std::runtime_error("Failed to generate random salt");
        }
    } else if (salt.size() == SALT_SIZE) {
        std::copy_n(salt.data(), SALT_SIZE, salt_buf.begin());
    } else {
        throw std::runtime_error("Salt must be 16 bytes");
    }

    std::array<uint8_t, NONCE_SIZE> nonce_buf{};
    if (RAND_bytes(nonce_buf.data(), static_cast<int>(NONCE_SIZE)) != 1) {
        throw std::runtime_error("Failed to generate random nonce");
    }

    std::vector<uint8_t> ct;
    std::array<uint8_t, TAG_SIZE> tag{};
    encrypt_gcm_block(data, key, nonce_buf, ct, tag);

    // Format: [salt:16][nonce:12][ciphertext:...][tag:16] (44 bytes overhead)
    std::vector<uint8_t> result;
    result.reserve(SALT_SIZE + NONCE_SIZE + ct.size() + TAG_SIZE);
    result.insert(result.end(), salt_buf.begin(), salt_buf.end());
    result.insert(result.end(), nonce_buf.begin(), nonce_buf.end());
    result.insert(result.end(), ct.begin(), ct.end());
    result.insert(result.end(), tag.begin(), tag.end());
    return result;
}

// ── Chunk Decryption (Dual Wire Format) ────────────────────────────────

std::vector<uint8_t> decrypt_chunk(
    std::span<const uint8_t> ciphertext,
    std::span<const uint8_t> key
) {
    if (key.size() != KEY_SIZE) {
        throw std::runtime_error("Key must be 32 bytes for AES-256");
    }
    if (ciphertext.size() < LEGACY_OVERHEAD) {
        throw std::runtime_error("Ciphertext too short: " + std::to_string(ciphertext.size()) + " < 28 bytes");
    }

    std::vector<uint8_t> pt;

    // Dual wire format: try 44-byte format first if size >= 44
    if (ciphertext.size() >= WIRE_OVERHEAD) {
        auto nonce = ciphertext.subspan(SALT_SIZE, NONCE_SIZE);
        auto ct = ciphertext.subspan(HEADER_SIZE, ciphertext.size() - WIRE_OVERHEAD);
        auto tag = ciphertext.subspan(ciphertext.size() - TAG_SIZE, TAG_SIZE);

        if (decrypt_gcm_block(ct, tag, key, nonce, pt)) {
            return pt;
        }
    }

    // Fallback: try legacy 28-byte format ([nonce:12][ct][tag:16])
    auto nonce = ciphertext.subspan(0, NONCE_SIZE);
    auto ct = ciphertext.subspan(NONCE_SIZE, ciphertext.size() - LEGACY_OVERHEAD);
    auto tag = ciphertext.subspan(ciphertext.size() - TAG_SIZE, TAG_SIZE);

    if (decrypt_gcm_block(ct, tag, key, nonce, pt)) {
        return pt;
    }

    throw std::runtime_error("Decryption failed — authentication tag mismatch");
}

// ── Password-Based Chunk Encryption & Decryption ──────────────────────

std::vector<uint8_t> encrypt_chunk(
    std::span<const uint8_t> data,
    std::string_view password,
    std::span<const uint8_t> salt
) {
    std::array<uint8_t, SALT_SIZE> salt_buf{};
    if (salt.empty()) {
        if (RAND_bytes(salt_buf.data(), static_cast<int>(SALT_SIZE)) != 1) {
            throw std::runtime_error("Failed to generate random salt");
        }
    } else if (salt.size() == SALT_SIZE) {
        std::copy_n(salt.data(), SALT_SIZE, salt_buf.begin());
    } else {
        throw std::runtime_error("Salt must be 16 bytes");
    }

    auto key = derive_key(password, salt_buf);
    return encrypt_chunk(data, key, salt_buf);
}

std::vector<uint8_t> decrypt_chunk(
    std::span<const uint8_t> ciphertext,
    std::string_view password,
    std::span<const uint8_t> fallback_salt
) {
    if (ciphertext.size() < LEGACY_OVERHEAD) {
        throw std::runtime_error("Ciphertext too short: " + std::to_string(ciphertext.size()) + " < 28 bytes");
    }

    // Dual wire format: 44-byte format with embedded salt
    if (ciphertext.size() >= WIRE_OVERHEAD) {
        auto salt = ciphertext.subspan(0, SALT_SIZE);
        auto key = derive_key(password, salt);
        auto nonce = ciphertext.subspan(SALT_SIZE, NONCE_SIZE);
        auto ct = ciphertext.subspan(HEADER_SIZE, ciphertext.size() - WIRE_OVERHEAD);
        auto tag = ciphertext.subspan(ciphertext.size() - TAG_SIZE, TAG_SIZE);

        std::vector<uint8_t> pt;
        if (decrypt_gcm_block(ct, tag, key, nonce, pt)) {
            return pt;
        }
    }

    // Legacy 28-byte format requires fallback salt
    if (fallback_salt.empty()) {
        throw std::runtime_error("Legacy chunk requires fallback salt for password key derivation");
    }

    auto key = derive_key(password, fallback_salt);
    auto nonce = ciphertext.subspan(0, NONCE_SIZE);
    auto ct = ciphertext.subspan(NONCE_SIZE, ciphertext.size() - LEGACY_OVERHEAD);
    auto tag = ciphertext.subspan(ciphertext.size() - TAG_SIZE, TAG_SIZE);

    std::vector<uint8_t> pt;
    if (decrypt_gcm_block(ct, tag, key, nonce, pt)) {
        return pt;
    }

    throw std::runtime_error("Decryption failed — authentication tag mismatch");
}

// ── File Helpers ──────────────────────────────────────────────────────

void encrypt_file_simple(
    const std::string& input_path,
    const std::string& output_path,
    std::string_view password
) {
    std::ifstream in(input_path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open input file: " + input_path);
    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
    in.close();

    auto encrypted = encrypt_chunk(buffer, password);

    std::ofstream out(output_path, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot open output file: " + output_path);
    out.write(reinterpret_cast<const char*>(encrypted.data()), static_cast<std::streamsize>(encrypted.size()));
}

void decrypt_file_simple(
    const std::string& input_path,
    const std::string& output_path,
    std::string_view password
) {
    std::ifstream in(input_path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open input file: " + input_path);
    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
    in.close();

    auto decrypted = decrypt_chunk(buffer, password);

    std::ofstream out(output_path, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot open output file: " + output_path);
    out.write(reinterpret_cast<const char*>(decrypted.data()), static_cast<std::streamsize>(decrypted.size()));
}

} // namespace tv
