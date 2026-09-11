#include <gtest/gtest.h>
#include <vector>
#include <span>
#include <cstdint>
#include <array>
#include <string>
#include <openssl/evp.h>

// Include the actual crypto implementation
#include "crypto/aes256gcm.hpp"
#include "crypto/kdf.hpp"
#include "crypto/stream.hpp"

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

    // Ciphertext should be salt(16) + nonce(12) + encrypted + tag(16) = plaintext + 44
    EXPECT_EQ(ciphertext.size(), plaintext.size() + 44);
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

    // Nonces should differ (bytes 16..27) and salts should differ (bytes 0..15)
    bool nonces_differ = false;
    for (size_t i = 16; i < 28; ++i) {
        if (ct1[i] != ct2[i]) { nonces_differ = true; break; }
    }
    EXPECT_TRUE(nonces_differ);
}

TEST(CryptoTest, WireFormat44BytesAndZeroLength) {
    std::vector<uint8_t> key(32, 0xEE);
    std::vector<uint8_t> empty;
    auto ct = encrypt_chunk(empty, key);
    EXPECT_EQ(ct.size(), 44);
    auto pt = decrypt_chunk(ct, key);
    EXPECT_TRUE(pt.empty());
}

TEST(CryptoTest, EncryptionHeaderSerialization) {
    auto hdr = EncryptionHeader::generate();
    auto bytes = hdr.to_bytes();
    EXPECT_EQ(bytes.size(), EncryptionHeader::SIZE);
    auto parsed = EncryptionHeader::from_bytes(bytes);
    EXPECT_EQ(parsed.salt, hdr.salt);
    EXPECT_EQ(parsed.nonce, hdr.nonce);
}

TEST(CryptoTest, DualWireFormatLegacySupport) {
    std::vector<uint8_t> key(32, 0x55);
    std::vector<uint8_t> plaintext = {'L', 'e', 'g', 'a', 'c', 'y', '!'};
    
    // Manually construct legacy 28-byte chunk: [nonce:12][ct][tag:16]
    std::array<uint8_t, 12> legacy_nonce{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr), 1);
    EXPECT_EQ(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr), 1);
    EXPECT_EQ(EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), legacy_nonce.data()), 1);
    
    std::vector<uint8_t> ct(plaintext.size() + 16);
    int len = 0, ct_len = 0;
    EXPECT_EQ(EVP_EncryptUpdate(ctx, ct.data(), &len, plaintext.data(), static_cast<int>(plaintext.size())), 1);
    ct_len = len;
    EXPECT_EQ(EVP_EncryptFinal_ex(ctx, ct.data() + ct_len, &len), 1);
    ct_len += len;
    ct.resize(ct_len);
    
    std::array<uint8_t, 16> tag{};
    EXPECT_EQ(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()), 1);
    EVP_CIPHER_CTX_free(ctx);
    
    std::vector<uint8_t> legacy_payload;
    legacy_payload.insert(legacy_payload.end(), legacy_nonce.begin(), legacy_nonce.end());
    legacy_payload.insert(legacy_payload.end(), ct.begin(), ct.end());
    legacy_payload.insert(legacy_payload.end(), tag.begin(), tag.end());
    
    EXPECT_EQ(legacy_payload.size(), plaintext.size() + 28);
    
    auto decrypted = decrypt_chunk(legacy_payload, key);
    EXPECT_EQ(decrypted, plaintext);
}

TEST(CryptoTest, StreamingEncryptDecryptRoundTrip) {
    std::vector<uint8_t> key(32, 0x77);
    std::array<uint8_t, 12> base_nonce{10, 11, 12, 13, 14, 15, 16, 17, 0, 0, 0, 0};
    
    StreamingEncryptor enc(key, base_nonce);
    StreamingDecryptor dec(key, base_nonce);
    
    std::vector<uint8_t> b1 = {'B', 'l', 'o', 'c', 'k', '1'};
    std::vector<uint8_t> b2 = {'B', 'l', 'o', 'c', 'k', '2', '!'};
    
    auto eb1 = enc.process(b1);
    auto eb2 = enc.process(b2);
    
    EXPECT_EQ(eb1.size(), b1.size() + 16);
    EXPECT_EQ(eb2.size(), b2.size() + 16);
    
    auto db1 = dec.process(eb1);
    auto db2 = dec.process(eb2);
    EXPECT_EQ(db1, b1);
    EXPECT_EQ(db2, b2);
}

TEST(CryptoTest, StreamingOutOfOrderRejection) {
    std::vector<uint8_t> key(32, 0x88);
    std::array<uint8_t, 12> base_nonce{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    
    StreamingEncryptor enc(key, base_nonce);
    StreamingDecryptor dec(key, base_nonce);
    
    auto eb0 = enc.process(std::vector<uint8_t>{'A', 'A'});
    auto eb1 = enc.process(std::vector<uint8_t>{'B', 'B'});
    
    // Attempting to decrypt eb1 when expecting block 0 must throw
    EXPECT_THROW(dec.process(eb1), std::runtime_error);
}

TEST(CryptoTest, PBKDF2KeyDerivationParity) {
    std::string pass = "password";
    std::vector<uint8_t> salt = {'s', 'a', 'l', 't', '1', '2', '3', '4', '5', '6', '7', '8'};
    auto key = derive_key_pbkdf2(pass, salt, 100000);
    EXPECT_EQ(key.size(), 32);
    
    // Expected hex: 73E81BDF8A029687E80A3766F2E19D4D1057D4CC3385842BE82A6904E559DA5E
    EXPECT_EQ(key[0], 0x73);
    EXPECT_EQ(key[1], 0xE8);
    EXPECT_EQ(key[31], 0x5E);
}

TEST(CryptoTest, Argon2idKeyDerivationParity) {
    std::string pass = "password";
    std::vector<uint8_t> salt = {'s', 'a', 'l', 't', '1', '2', '3', '4', '5', '6', '7', '8'};
    try {
        auto key = derive_key_argon2id(pass, salt, 1, 65536, 1);
        EXPECT_EQ(key.size(), 32);
        // Expected hex: 449428F91BF4B8E574B5CC77696718723C97523ACF4B051B7B9372C60EC4DD9D
        EXPECT_EQ(key[0], 0x44);
        EXPECT_EQ(key[1], 0x94);
        EXPECT_EQ(key[31], 0x9D);
    } catch (const std::runtime_error& e) {
        // OpenSSL < 3.2 (e.g. Ubuntu 24.04's 3.0.x) ships no ARGON2ID
        // provider; scrypt remains the default KDF there.
        GTEST_SKIP() << "ARGON2ID not available on this OpenSSL: " << e.what();
    }
}

TEST(CryptoTest, PasswordBasedEncryptDecrypt) {
    std::string pass = "vault_master_pass";
    std::vector<uint8_t> plaintext = {'S', 'e', 'c', 'r', 'e', 't', '!'};
    
    auto ct = encrypt_chunk(plaintext, pass);
    EXPECT_EQ(ct.size(), plaintext.size() + 44);
    
    auto pt = decrypt_chunk(ct, pass);
    EXPECT_EQ(pt, plaintext);
    
    EXPECT_THROW(decrypt_chunk(ct, "wrong_pass"), std::runtime_error);
}

TEST(CryptoTest, FallbackSaltDualWireFormat) {
    std::string pass = "vault_master_pass";
    std::vector<uint8_t> fallback_salt = {'t','e','l','e','v','a','u','l','t','1','2','3','4','5','6','7'};
    std::vector<uint8_t> plaintext = {'F', 'a', 'l', 'l', 'b', 'a', 'c', 'k', '!'};
    
    // Encrypt with key derived from fallback_salt (simulating channel-derived master_key)
    auto key = derive_key(pass, fallback_salt);
    auto ct = encrypt_chunk(plaintext, key); // Writes 44-byte format with random salt
    
    // Decrypting with wrong password must throw
    EXPECT_THROW(decrypt_chunk(ct, "wrong_pass", fallback_salt), std::runtime_error);
    
    // Decrypting with correct password and fallback_salt must succeed via fallback!
    auto pt = decrypt_chunk(ct, pass, fallback_salt);
    EXPECT_EQ(pt, plaintext);
}
