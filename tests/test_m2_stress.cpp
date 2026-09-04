#include <gtest/gtest.h>
#include <vector>
#include <span>
#include <cstdint>
#include <array>
#include <string>
#include <random>
#include <algorithm>
#include <zstd.h>
#include <openssl/evp.h>

#include "crypto/aes256gcm.hpp"
#include "crypto/kdf.hpp"
#include "crypto/stream.hpp"
#include "compress/zstd.hpp"
#include "compress/stream.hpp"

using namespace tv;

namespace {

std::vector<uint8_t> generate_pseudo_data(size_t size, uint32_t seed = 42) {
    std::vector<uint8_t> data(size);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>(dist(rng));
    }
    return data;
}

std::vector<uint8_t> compress_without_contentsize(std::span<const uint8_t> data, int level = 3) {
    auto cctx = ZSTD_createCCtx();
    if (!cctx) throw std::runtime_error("Failed to create ZSTD CCtx");
    
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
    // Explicitly disable writing the content size in frame header
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0);

    size_t bound = ZSTD_compressBound(data.size());
    std::vector<uint8_t> out(bound);

    ZSTD_inBuffer in_buf{data.data(), data.size(), 0};
    ZSTD_outBuffer out_buf{out.data(), out.size(), 0};

    size_t ret = ZSTD_compressStream2(cctx, &out_buf, &in_buf, ZSTD_e_end);
    if (ZSTD_isError(ret)) {
        ZSTD_freeCCtx(cctx);
        throw std::runtime_error(std::string("ZSTD error: ") + ZSTD_getErrorName(ret));
    }
    ZSTD_freeCCtx(cctx);
    out.resize(out_buf.pos);
    return out;
}

} // namespace

// ============================================================================
// 1. Streaming Crypto Stress: Out-of-Order Block Injection & Replay Attacks
// ============================================================================

TEST(M2StressTest, StreamingCryptoOutOfOrderInjection) {
    std::vector<uint8_t> key(32, 0x33);
    std::array<uint8_t, 12> base_nonce{1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0};

    StreamingEncryptor enc(key, base_nonce);
    
    // Encrypt 5 distinct blocks
    std::vector<std::vector<uint8_t>> plaintexts = {
        {'P', 'a', 'y', 'l', 'o', 'a', 'd', ' ', '0'},
        {'P', 'a', 'y', 'l', 'o', 'a', 'd', ' ', '1'},
        {'P', 'a', 'y', 'l', 'o', 'a', 'd', ' ', '2'},
        {'P', 'a', 'y', 'l', 'o', 'a', 'd', ' ', '3'},
        {'P', 'a', 'y', 'l', 'o', 'a', 'd', ' ', '4'}
    };

    std::vector<std::vector<uint8_t>> ciphertexts;
    for (const auto& pt : plaintexts) {
        ciphertexts.push_back(enc.process(pt));
    }

    // Attack 1: Out-of-order block injection at start (feed block 1 when expecting block 0)
    {
        StreamingDecryptor dec(key, base_nonce);
        EXPECT_THROW(dec.process(ciphertexts[1]), std::runtime_error);
    }

    // Attack 2: Out-of-order block injection mid-stream (feed block 0, then block 3 when expecting block 1)
    {
        StreamingDecryptor dec(key, base_nonce);
        auto d0 = dec.process(ciphertexts[0]);
        EXPECT_EQ(d0, plaintexts[0]);
        EXPECT_THROW(dec.process(ciphertexts[3]), std::runtime_error);
    }

    // Attack 3: Reverse-order block injection
    {
        StreamingDecryptor dec(key, base_nonce);
        EXPECT_THROW(dec.process(ciphertexts[4]), std::runtime_error);
    }

    // Legitimate in-order processing must succeed
    {
        StreamingDecryptor dec(key, base_nonce);
        for (size_t i = 0; i < plaintexts.size(); ++i) {
            auto pt = dec.process(ciphertexts[i]);
            EXPECT_EQ(pt, plaintexts[i]);
        }
    }
}

TEST(M2StressTest, StreamingCryptoReplayAttacks) {
    std::vector<uint8_t> key(32, 0x44);
    std::array<uint8_t, 12> base_nonce{9, 8, 7, 6, 5, 4, 3, 2, 0, 0, 0, 0};

    StreamingEncryptor enc(key, base_nonce);
    
    std::vector<uint8_t> b0{'B', 'l', 'o', 'c', 'k', '_', '0'};
    std::vector<uint8_t> b1{'B', 'l', 'o', 'c', 'k', '_', '1'};
    std::vector<uint8_t> b2{'B', 'l', 'o', 'c', 'k', '_', '2'};

    auto eb0 = enc.process(b0);
    auto eb1 = enc.process(b1);
    auto eb2 = enc.process(b2);

    // Attack 1: Replay block 0 immediately after block 0
    {
        StreamingDecryptor dec(key, base_nonce);
        EXPECT_EQ(dec.process(eb0), b0);
        EXPECT_THROW(dec.process(eb0), std::runtime_error);
    }

    // Attack 2: Replay block 0 after block 1
    {
        StreamingDecryptor dec(key, base_nonce);
        EXPECT_EQ(dec.process(eb0), b0);
        EXPECT_EQ(dec.process(eb1), b1);
        EXPECT_THROW(dec.process(eb0), std::runtime_error);
    }

    // Attack 3: Replay block 1 after block 2
    {
        StreamingDecryptor dec(key, base_nonce);
        EXPECT_EQ(dec.process(eb0), b0);
        EXPECT_EQ(dec.process(eb1), b1);
        EXPECT_EQ(dec.process(eb2), b2);
        EXPECT_THROW(dec.process(eb1), std::runtime_error);
    }
}

TEST(M2StressTest, StreamingCryptoTruncatedAndCorruptedBlocks) {
    std::vector<uint8_t> key(32, 0x55);
    std::array<uint8_t, 12> base_nonce{1, 3, 5, 7, 9, 11, 13, 15, 0, 0, 0, 0};

    StreamingEncryptor enc(key, base_nonce);
    std::vector<uint8_t> payload = {'S', 't', 'r', 'e', 'a', 'm', ' ', 'D', 'a', 't', 'a'};
    auto eb = enc.process(payload);

    // Truncated block (< 16 bytes tag)
    {
        StreamingDecryptor dec(key, base_nonce);
        std::vector<uint8_t> truncated(eb.begin(), eb.begin() + 10);
        EXPECT_THROW(dec.process(truncated), std::runtime_error);
    }

    // Corrupted tag
    {
        StreamingDecryptor dec(key, base_nonce);
        auto corrupted = eb;
        corrupted.back() ^= 0xAA;
        EXPECT_THROW(dec.process(corrupted), std::runtime_error);
    }

    // Corrupted ciphertext body
    {
        StreamingDecryptor dec(key, base_nonce);
        auto corrupted = eb;
        corrupted[2] ^= 0x55;
        EXPECT_THROW(dec.process(corrupted), std::runtime_error);
    }
}

TEST(M2StressTest, StreamingCryptoManyBlocksStress) {
    std::vector<uint8_t> key(32, 0x66);
    std::array<uint8_t, 12> base_nonce{0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0, 0, 0, 0};

    StreamingEncryptor enc(key, base_nonce);
    StreamingDecryptor dec(key, base_nonce);

    // 128 blocks of alternating sizes
    for (uint32_t i = 0; i < 128; ++i) {
        size_t size = (i % 7 + 1) * 256;
        auto pt = generate_pseudo_data(size, i);
        auto ct = enc.process(pt);
        EXPECT_EQ(ct.size(), pt.size() + 16);
        auto decrypted = dec.process(ct);
        EXPECT_EQ(decrypted, pt);
    }
}

// ============================================================================
// 2. Compression Stress: ZSTD_CONTENTSIZE_UNKNOWN & Multi-MB Streams
// ============================================================================

TEST(M2StressTest, CompressionUnknownContentSizeVariousSizes) {
    std::vector<size_t> test_sizes = {16, 512, 4096, 65536, 1024 * 1024, 4 * 1024 * 1024};

    for (size_t size : test_sizes) {
        auto original = generate_pseudo_data(size, static_cast<uint32_t>(size));
        
        // Compress with content size header explicitly disabled
        auto compressed = compress_without_contentsize(original);
        
        // Confirm frame header returns unknown content size
        auto detected_size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
        EXPECT_EQ(detected_size, ZSTD_CONTENTSIZE_UNKNOWN) << "Size was not unknown for len=" << size;

        // decompress_data must transparently stream-decompress and return exact original
        auto decompressed = decompress_data(compressed);
        EXPECT_EQ(decompressed.size(), original.size());
        EXPECT_EQ(decompressed, original);
    }
}

TEST(M2StressTest, CompressionStreamingMultiMegabyteStress) {
    const size_t total_size = 10 * 1024 * 1024; // 10MB
    std::vector<uint8_t> original(total_size);
    // Fill with pattern that compresses moderately well
    for (size_t i = 0; i < total_size; ++i) {
        original[i] = static_cast<uint8_t>((i % 128) ^ ((i / 1024) % 64));
    }

    StreamingCompressor compressor(3);
    std::vector<uint8_t> compressed;

    // Feed in small, odd chunk sizes (e.g. 1337 bytes)
    size_t chunk_in_size = 1337;
    for (size_t offset = 0; offset < total_size; offset += chunk_in_size) {
        size_t len = std::min(chunk_in_size, total_size - offset);
        auto out = compressor.process(std::span<const uint8_t>(original.data() + offset, len));
        compressed.insert(compressed.end(), out.begin(), out.end());
    }
    auto final_comp = compressor.finalize();
    compressed.insert(compressed.end(), final_comp.begin(), final_comp.end());

    EXPECT_LT(compressor.ratio(), 0.5);
    EXPECT_EQ(compressor.total_in(), total_size);
    EXPECT_EQ(compressor.total_out(), compressed.size());

    // Decompress in completely different odd chunk sizes (e.g. 7777 bytes)
    StreamingDecompressor decompressor;
    std::vector<uint8_t> decompressed;
    size_t chunk_out_size = 7777;

    for (size_t offset = 0; offset < compressed.size(); offset += chunk_out_size) {
        size_t len = std::min(chunk_out_size, compressed.size() - offset);
        auto out = decompressor.process(std::span<const uint8_t>(compressed.data() + offset, len));
        decompressed.insert(decompressed.end(), out.begin(), out.end());
    }
    auto final_dec = decompressor.finalize();
    decompressed.insert(decompressed.end(), final_dec.begin(), final_dec.end());

    EXPECT_EQ(decompressed.size(), original.size());
    EXPECT_EQ(decompressed, original);
}

// ============================================================================
// 3. Cryptographic Tampering Rejection Parity
// ============================================================================

TEST(M2StressTest, DualWireFormatTamperRejection) {
    std::vector<uint8_t> key(32, 0x77);
    std::vector<uint8_t> plaintext = {'H', 'a', 'r', 'd', 'e', 'n', 'e', 'd', ' ', 'D', 'a', 't', 'a'};

    // 44-byte format
    auto ct44 = encrypt_chunk(plaintext, key);
    ASSERT_EQ(ct44.size(), plaintext.size() + 44);

    // Key-based decryption: Nonce (16..27), Ciphertext (28..end-16), Tag (end-16..end)
    // Nonce tampering
    for (size_t i = 16; i < 28; i += 3) {
        auto tampered = ct44;
        tampered[i] ^= 0x01;
        EXPECT_THROW(decrypt_chunk(tampered, key), std::runtime_error);
    }

    // Ciphertext body tampering
    for (size_t i = 28; i < ct44.size() - 16; ++i) {
        auto tampered = ct44;
        tampered[i] ^= 0x01;
        EXPECT_THROW(decrypt_chunk(tampered, key), std::runtime_error);
    }

    // Tag tampering
    for (size_t i = ct44.size() - 16; i < ct44.size(); ++i) {
        auto tampered = ct44;
        tampered[i] ^= 0x01;
        EXPECT_THROW(decrypt_chunk(tampered, key), std::runtime_error);
    }

    // Sub-28-byte truncated ciphertext
    for (size_t len = 0; len < 28; ++len) {
        std::vector<uint8_t> short_ct(ct44.begin(), ct44.begin() + len);
        EXPECT_THROW(decrypt_chunk(short_ct, key), std::runtime_error);
    }

    // Password-based decryption: Salt IS authenticated via KDF key derivation!
    std::string password = "strong_test_password_42";
    auto ct44_pass = encrypt_chunk(plaintext, password);
    
    // Salt tampering (0..15) must fail authentication in password mode
    for (size_t i = 0; i < 16; i += 3) {
        auto tampered = ct44_pass;
        tampered[i] ^= 0x01;
        EXPECT_THROW(decrypt_chunk(tampered, password), std::runtime_error);
    }

    // Nonce tampering in password mode
    for (size_t i = 16; i < 28; i += 3) {
        auto tampered = ct44_pass;
        tampered[i] ^= 0x01;
        EXPECT_THROW(decrypt_chunk(tampered, password), std::runtime_error);
    }

    // Ciphertext tampering in password mode
    for (size_t i = 28; i < ct44_pass.size() - 16; ++i) {
        auto tampered = ct44_pass;
        tampered[i] ^= 0x01;
        EXPECT_THROW(decrypt_chunk(tampered, password), std::runtime_error);
    }

    // Tag tampering in password mode
    for (size_t i = ct44_pass.size() - 16; i < ct44_pass.size(); ++i) {
        auto tampered = ct44_pass;
        tampered[i] ^= 0x01;
        EXPECT_THROW(decrypt_chunk(tampered, password), std::runtime_error);
    }

    // Wrong password must fail authentication
    EXPECT_THROW(decrypt_chunk(ct44_pass, "wrong_password"), std::runtime_error);
}
