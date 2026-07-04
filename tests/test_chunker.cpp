#include <gtest/gtest.h>
#include <vector>
#include <span>
#include <cstdint>
#include <string>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>

#include "chunker/chunker.hpp"
#include "chunker/hash.hpp"
#include "chunker/writer.hpp"

using namespace tv;

TEST(ChunkerTest, HashData) {
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o'};
    auto hash = hash_data(data);
    EXPECT_EQ(hash.size(), 64); // BLAKE3 hex hash is 64 chars
    EXPECT_TRUE(std::all_of(hash.begin(), hash.end(), [](char c) {
        return std::isxdigit(c);
    }));
}

TEST(ChunkerTest, HashIsDeterministic) {
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o'};
    auto h1 = hash_data(data);
    auto h2 = hash_data(data);
    EXPECT_EQ(h1, h2);
}

TEST(ChunkerTest, HashFile) {
    auto tmp = std::filesystem::temp_directory_path() / "tv_test_hash.txt";
    std::ofstream f(tmp);
    f << "test data for hashing";
    f.close();

    auto hash = hash_file(tmp.string());
    EXPECT_EQ(hash.size(), 64);
    std::filesystem::remove(tmp);
}

TEST(ChunkerTest, ChunkFile) {
    auto tmp = std::filesystem::temp_directory_path() / "tv_test_chunk.bin";
    std::vector<uint8_t> test_data(1000);
    for (int i = 0; i < 1000; ++i) test_data[i] = static_cast<uint8_t>(i & 0xFF);

    std::ofstream f(tmp, std::ios::binary);
    f.write(reinterpret_cast<const char*>(test_data.data()), test_data.size());
    f.close();

    auto chunks = iter_chunks(tmp.string(), 300); // 300 byte chunks
    EXPECT_EQ(chunks.size(), 4); // 300+300+300+100

    uint64_t total = 0;
    for (auto& chunk : chunks) {
        EXPECT_FALSE(chunk.hash.empty());
        total += chunk.data.size();
    }
    EXPECT_EQ(total, 1000);

    // Verify chunk hashes
    for (auto& chunk : chunks) {
        auto computed = hash_data(chunk.data);
        EXPECT_EQ(computed, chunk.hash);
    }

    std::filesystem::remove(tmp);
}

TEST(ChunkerTest, ChunkWriter) {
    auto tmp = std::filesystem::temp_directory_path() / "tv_test_writer.bin";

    {
        ChunkWriter writer(tmp.string(), 256);
        std::vector<uint8_t> data1(100, 0xAB);
        std::vector<uint8_t> data2(100, 0xCD);
        std::vector<uint8_t> data3(56, 0xEF);

        // Write out of order
        writer.write(1, 100, data2.data(), data2.size());
        writer.write(0, 0, data1.data(), data1.size());
        writer.write(2, 200, data3.data(), data3.size());
    }

    // Verify content
    std::ifstream f(tmp, std::ios::binary);
    std::vector<uint8_t> content(256);
    f.read(reinterpret_cast<char*>(content.data()), 256);

    for (int i = 0; i < 100; ++i) EXPECT_EQ(content[i], 0xAB);
    for (int i = 100; i < 200; ++i) EXPECT_EQ(content[i], 0xCD);
    for (int i = 200; i < 256; ++i) EXPECT_EQ(content[i], 0xEF);

    std::filesystem::remove(tmp);
}

TEST(ChunkerTest, HashDifferentData) {
    std::vector<uint8_t> d1 = {'H', 'e', 'l', 'l', 'o'};
    std::vector<uint8_t> d2 = {'H', 'e', 'l', 'l', '!'};
    EXPECT_NE(hash_data(d1), hash_data(d2));
}
