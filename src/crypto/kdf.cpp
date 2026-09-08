#include "kdf.hpp"
#include <cstring>
#include <stdexcept>
#include <memory>
#include <limits>
#include <vector>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/core_names.h>

// OpenSSL 3.0 (Ubuntu 24.04, Debian 12) lacks ARGON2 OSSL names added in
// 3.2. Define fallbacks so portable release builds still compile; at
// runtime EVP_KDF_fetch("ARGON2ID") returns nullptr on 3.0 and the
// function throws, with scrypt/PBKDF2 as the working default.
#ifndef OSSL_KDF_PARAM_ARGON2_MEMCOST
#define OSSL_KDF_PARAM_ARGON2_MEMCOST "memcost"
#endif
#ifndef OSSL_KDF_PARAM_ARGON2_LANES
#define OSSL_KDF_PARAM_ARGON2_LANES "lanes"
#endif

namespace tv {

static constexpr uint64_t SCRYPT_N = 1 << 17;  // 131072
static constexpr uint32_t SCRYPT_R = 8;
static constexpr uint32_t SCRYPT_P = 1;
static constexpr uint64_t SCRYPT_MAXMEM = 256ULL * 1024 * 1024; // 256 MB

struct EvpKdfDeleter {
    void operator()(EVP_KDF* kdf) const noexcept {
        if (kdf) EVP_KDF_free(kdf);
    }
};
using UniqueEvpKdf = std::unique_ptr<EVP_KDF, EvpKdfDeleter>;

struct EvpKdfCtxDeleter {
    void operator()(EVP_KDF_CTX* ctx) const noexcept {
        if (ctx) EVP_KDF_CTX_free(ctx);
    }
};
using UniqueEvpKdfCtx = std::unique_ptr<EVP_KDF_CTX, EvpKdfCtxDeleter>;

std::array<uint8_t, 32> derive_key_scrypt(
    std::string_view password,
    std::span<const uint8_t> salt,
    uint64_t n,
    uint32_t r,
    uint32_t p
) {
    if (password.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        salt.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Password or salt length exceeds maximum supported size");
    }

    std::array<uint8_t, 32> key{};

    int rc = EVP_PBE_scrypt(
        password.data(), password.size(),
        salt.data(), salt.size(),
        n, r, p,
        SCRYPT_MAXMEM,
        key.data(), key.size()
    );

    if (rc != 1) {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        throw std::runtime_error(std::string("scrypt key derivation failed: ") + err_buf);
    }

    return key;
}

std::array<uint8_t, 32> derive_key_pbkdf2(
    std::string_view password,
    std::span<const uint8_t> salt,
    uint32_t iterations
) {
    if (password.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        salt.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Password or salt length exceeds maximum supported size");
    }

    std::array<uint8_t, 32> key{};

    int rc = PKCS5_PBKDF2_HMAC(
        password.data(), static_cast<int>(password.size()),
        salt.data(), static_cast<int>(salt.size()),
        static_cast<int>(iterations),
        EVP_sha256(),
        static_cast<int>(key.size()),
        key.data()
    );

    if (rc != 1) {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        throw std::runtime_error(std::string("pbkdf2 key derivation failed: ") + err_buf);
    }

    return key;
}

std::array<uint8_t, 32> derive_key_argon2id(
    std::string_view password,
    std::span<const uint8_t> salt,
    uint32_t iterations,
    uint32_t memory_kib,
    uint32_t parallelism
) {
    UniqueEvpKdf kdf(EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr));
    if (!kdf) {
        throw std::runtime_error("OpenSSL ARGON2ID KDF not available");
    }
    UniqueEvpKdfCtx kctx(EVP_KDF_CTX_new(kdf.get()));
    if (!kctx) {
        throw std::runtime_error("Failed to create ARGON2ID context");
    }

    std::vector<uint8_t> pass_buf(password.begin(), password.end());
    std::vector<uint8_t> salt_buf(salt.begin(), salt.end());

    OSSL_PARAM params[6];
    params[0] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, pass_buf.data(), pass_buf.size());
    params[1] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, salt_buf.data(), salt_buf.size());
    params[2] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iterations);
    params[3] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memory_kib);
    params[4] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &parallelism);
    params[5] = OSSL_PARAM_construct_end();

    std::array<uint8_t, 32> key{};
    if (EVP_KDF_derive(kctx.get(), key.data(), key.size(), params) <= 0) {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        throw std::runtime_error(std::string("argon2id key derivation failed: ") + err_buf);
    }

    return key;
}

std::array<uint8_t, 32> derive_key(std::string_view password, std::span<const uint8_t> salt) {
    return derive_key_scrypt(password, salt, SCRYPT_N, SCRYPT_R, SCRYPT_P);
}

} // namespace tv
