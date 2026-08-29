#include "kdf.hpp"
#include <cstring>
#include <stdexcept>
#include <openssl/evp.h>
#include <openssl/err.h>

namespace tv {

static constexpr uint64_t SCRYPT_N = 1 << 17;  // 131072
static constexpr uint32_t SCRYPT_R = 8;
static constexpr uint32_t SCRYPT_P = 1;
static constexpr uint64_t SCRYPT_MAXMEM = 256ULL * 1024 * 1024; // 256 MB

std::array<uint8_t, 32> derive_key(std::string_view password, std::span<const uint8_t> salt) {
    std::array<uint8_t, 32> key{};

    int rc = EVP_PBE_scrypt(
        password.data(), password.size(),
        salt.data(), salt.size(),
        SCRYPT_N, SCRYPT_R, SCRYPT_P,
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

} // namespace tv
