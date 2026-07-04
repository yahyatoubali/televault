#include <gtest/gtest.h>
#include <vector>
#include <span>
#include <cstdint>

// Include the actual crypto implementation
#include "crypto/aes256gcm.hpp"
#include "crypto/kdf.hpp"

using namespace tv;

TEST(CryptoTest, KeyDerivation) {
    std::vector<uint8_t> salt = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    auto key = derive_key("test_password", salt);
    EXPECT_EQ(key.size(), 32);

    // Deterministic with same inputs
    auto key2 = derive_key("test_password", salt);
    EXPECT_EQ(key, key2);

    // Different salt → different key
    std::vector<uint8_t> salt2 = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8,
                                  0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0xF1, 0xF0};
    auto key3 = derive_key("test_password", salt2);
    EXPECT_NE(key, key3);
}

TEST(CryptoTest, EncryptDecryptRoundTrip) {
    std::vector<uint8_t> key(32, 0xAB);
    std::vector<uint8_t> plaintext = {0x48, 0x65, 0x6C, 0x6C, 0x6F}; // "Hello"

    auto ciphertext = encrypt_chunk(plaintext, key);

    // Ciphertext should be nonce(12) + encrypted + tag(16)
    EXPECT_GT(ciphertext.size(), plaintext.size() + 12);
    EXPECT_NE(ciphertext, plaintext); // should be different

    auto decrypted = decrypt_chunk(ciphertext, key);
    EXPECT_EQ(decrypted, plaintext);
}

TEST(CryptoTest, DecryptFailsWithWrongKey) {
    std::vector<uint8_t> key(32, 0xAB);
    std::vector<uint8_t> wrong_key(32, 0xCD);
    std::vector<uint8_t> plaintext = {0x48, 0x65, 0x6C, 0x6C, 0x6F};

    auto ciphertext = encrypt_chunk(plaintext, key);
    EXPECT_THROW(decrypt_chunk(ciphertext, wrong_key), std::runtime_error);
}

TEST(CryptoTest, DecryptFailsWithCorruptedData) {
    std::vector<uint8_t> key(32, 0xAB);
    std::vector<uint8_t> plaintext = {0x48, 0x65, 0x6C, 0x6C, 0x6F};

    auto ciphertext = encrypt_chunk(plaintext, key);
    ciphertext[ciphertext.size() - 1] ^= 0xFF; // corrupt last byte (tag)

    EXPECT_THROW(decrypt_chunk(ciphertext, key), std::runtime_error);
}

TEST(CryptoTest, LargeDataRoundTrip) {
    std::vector<uint8_t> key(32, 0x42);
    std::vector<uint8_t> plaintext(1024 * 1024); // 1MB
    for (size_t i = 0; i < plaintext.size(); ++i) {
        plaintext[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto ciphertext = encrypt_chunk(plaintext, key);
    auto decrypted = decrypt_chunk(ciphertext, key);

    EXPECT_EQ(decrypted.size(), plaintext.size());
    EXPECT_EQ(decrypted, plaintext);
}

TEST(CryptoTest, UniqueNoncePerEncryption) {
    std::vector<uint8_t> key(32, 0xAB);
    std::vector<uint8_t> plaintext = {0x48, 0x65, 0x6C, 0x6C, 0x6F};

    auto ct1 = encrypt_chunk(plaintext, key);
    auto ct2 = encrypt_chunk(plaintext, key);

    // Nonces should differ (only first 12 bytes)
    bool nonces_differ = false;
    for (int i = 0; i < 12; ++i) {
        if (ct1[i] != ct2[i]) { nonces_differ = true; break; }
    }
    EXPECT_TRUE(nonces_differ);
}
