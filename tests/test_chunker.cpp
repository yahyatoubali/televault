#include <gtest/gtest.h>
#include <vector>
#include <span>
#include <cstdint>
#include <string>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <random>
#include <cstring>

#include "chunker/chunker.hpp"
#include "chunker/fastcdc.hpp"
#include "chunker/hash.hpp"
#include "chunker/writer.hpp"

using namespace tv;

TEST(ChunkerTest, HashData) {
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o'};
    auto hash = hash_data(data);
    EXPECT_EQ(hash.size(), 64u); // BLAKE3 hex hash is 64 chars
    EXPECT_TRUE(std::all_of(hash.begin(), hash.end(), [](char c) {
        return std::isxdigit(c);
    }));
}

TEST(ChunkerTest, HashData32Prefix) {
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o'};
    auto full = hash_data(data, 64);
    auto prefix = hash_data(data, 32);
    EXPECT_EQ(prefix.size(), 32u);
    EXPECT_EQ(full.substr(0, 32), prefix);

    auto prefix_helper = hash_data_prefix(data, 32);
    EXPECT_EQ(prefix_helper, prefix);
}

TEST(ChunkerTest, HashEmptyCanonical) {
    std::vector<uint8_t> empty;
    auto hash = hash_data(empty);
    EXPECT_EQ(hash, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
    auto prefix = hash_data_prefix(empty, 32);
    EXPECT_EQ(prefix, "af1349b9f5f9a1a6a0404dea36dcc949");
}

TEST(ChunkerTest, Blake3HasherIncremental) {
    std::vector<uint8_t> part1 = {'P', 'a', 'r', 't', ' ', '1'};
    std::vector<uint8_t> part2 = {':', ' ', 'D', 'a', 't', 'a'};
    std::vector<uint8_t> combined;
    combined.insert(combined.end(), part1.begin(), part1.end());
    combined.insert(combined.end(), part2.begin(), part2.end());

    Blake3Hasher hasher;
    hasher.update(part1);
    hasher.update(part2);
    auto streamed = hasher.finalize();

    auto one_shot = hash_data(combined);
    EXPECT_EQ(streamed, one_shot);
}

TEST(ChunkerTest, HashMatches) {
    std::string full = "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262";
    std::string prefix = "af1349b9f5f9a1a6a0404dea36dcc949";
    EXPECT_TRUE(hash_matches(full, prefix));
    EXPECT_TRUE(hash_matches(prefix, full));
    EXPECT_TRUE(hash_matches(full, full));
    EXPECT_FALSE(hash_matches(full, "0000000000000000"));
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
    EXPECT_EQ(hash.size(), 64u);

    auto prefix = hash_file_prefix(tmp.string(), 32);
    EXPECT_EQ(prefix.size(), 32u);
    EXPECT_EQ(hash.substr(0, 32), prefix);

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
    EXPECT_EQ(chunks.size(), 4u); // 300+300+300+100

    uint64_t total = 0;
    int64_t idx = 0;
    for (auto& chunk : chunks) {
        EXPECT_FALSE(chunk.hash.empty());
        EXPECT_EQ(chunk.index, idx++);
        total += chunk.data.size();
    }
    EXPECT_EQ(total, 1000u);

    // Verify chunk hashes
    for (auto& chunk : chunks) {
        auto computed = hash_data(chunk.data);
        EXPECT_EQ(computed, chunk.hash);
    }

    std::filesystem::remove(tmp);
}

TEST(ChunkerTest, ChunkerZeroByteFile) {
    auto tmp = std::filesystem::temp_directory_path() / "tv_test_zero.bin";
    std::ofstream f(tmp, std::ios::binary);
    f.close();

    auto stream = iter_chunks(tmp.string(), 1024);
    EXPECT_EQ(stream.size(), 0u);
    EXPECT_TRUE(stream.empty());
    EXPECT_EQ(stream.begin(), stream.end());

    int count = 0;
    for (auto& c : stream) {
        (void)c;
        ++count;
    }
    EXPECT_EQ(count, 0);

    std::filesystem::remove(tmp);
}

TEST(ChunkerTest, ChunkerExactSplit) {
    auto tmp = std::filesystem::temp_directory_path() / "tv_test_exact.bin";
    uint64_t chunk_size = 512;
    std::vector<uint8_t> test_data(chunk_size * 3, 0x42);

    std::ofstream f(tmp, std::ios::binary);
    f.write(reinterpret_cast<const char*>(test_data.data()), test_data.size());
    f.close();

    auto stream = iter_chunks(tmp.string(), chunk_size);
    EXPECT_EQ(stream.size(), 3u);

    size_t chunks_seen = 0;
    for (auto& c : stream) {
        EXPECT_EQ(c.data.size(), chunk_size);
        ++chunks_seen;
    }
    EXPECT_EQ(chunks_seen, 3u);

    std::filesystem::remove(tmp);
}

TEST(ChunkerTest, ChunkerCountAndRead) {
    EXPECT_EQ(count_chunks(0, 100), 0u);
    EXPECT_EQ(count_chunks(1, 100), 1u);
    EXPECT_EQ(count_chunks(100, 100), 1u);
    EXPECT_EQ(count_chunks(101, 100), 2u);
    EXPECT_EQ(count_chunks(250, 100), 3u);

    auto tmp = std::filesystem::temp_directory_path() / "tv_test_read.bin";
    std::vector<uint8_t> data = {'A', 'B', 'C', 'D', 'E', 'F'};
    std::ofstream f(tmp, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    f.close();

    auto c0 = read_chunk(tmp.string(), 0, 2);
    EXPECT_EQ(c0.index, 0);
    EXPECT_EQ(c0.data.size(), 2u);
    EXPECT_EQ(c0.data[0], 'A');
    EXPECT_EQ(c0.data[1], 'B');

    auto c2 = read_chunk(tmp.string(), 2, 2);
    EXPECT_EQ(c2.index, 2);
    EXPECT_EQ(c2.data.size(), 2u);
    EXPECT_EQ(c2.data[0], 'E');
    EXPECT_EQ(c2.data[1], 'F');

    EXPECT_THROW((void)read_chunk(tmp.string(), 3, 2), std::out_of_range);

    std::filesystem::remove(tmp);
}

TEST(ChunkerTest, ChunkFilenameFormat) {
    Chunk c;
    c.index = 0;
    EXPECT_EQ(c.filename(), "0000.chunk");
    c.index = 42;
    EXPECT_EQ(c.filename(), "0042.chunk");
    c.index = 1005;
    EXPECT_EQ(c.filename(), "1005.chunk");
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

        EXPECT_TRUE(writer.is_complete());
        EXPECT_TRUE(writer.is_complete(3));
        EXPECT_TRUE(writer.missing_chunks(3).empty());
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

TEST(ChunkerTest, ChunkWriterParentDirs) {
    auto nested = std::filesystem::temp_directory_path() / "nested_tv_test" / "sub" / "out.bin";
    std::error_code ec;
    std::filesystem::remove_all(nested.parent_path().parent_path(), ec);

    {
        ChunkWriter writer(nested.string(), 10);
        std::vector<uint8_t> data(10, 0x77);
        writer.write(0, 0, data.data(), data.size());
        EXPECT_TRUE(writer.is_complete());
    }

    EXPECT_TRUE(std::filesystem::exists(nested));
    EXPECT_EQ(std::filesystem::file_size(nested), 10u);
    std::filesystem::remove_all(nested.parent_path().parent_path(), ec);
}

TEST(ChunkerTest, ChunkWriterDuplicateChunkHandling) {
    auto tmp = std::filesystem::temp_directory_path() / "tv_test_dup.bin";

    {
        ChunkWriter writer(tmp.string(), 100);
        std::vector<uint8_t> chunk(100, 0x55);
        writer.write(0, 0, chunk.data(), chunk.size());
        EXPECT_EQ(writer.bytes_written(), 100u);

        // Duplicate write should be ignored
        writer.write(0, 0, chunk.data(), chunk.size());
        EXPECT_EQ(writer.bytes_written(), 100u);
        EXPECT_TRUE(writer.is_complete());
    }

    std::filesystem::remove(tmp);
}

TEST(ChunkerTest, ChunkWriterZeroByteFile) {
    auto tmp = std::filesystem::temp_directory_path() / "tv_test_writer_zero.bin";

    {
        ChunkWriter writer(tmp.string(), 0);
        EXPECT_TRUE(writer.is_complete());
        EXPECT_TRUE(writer.is_complete(0));
    }

    EXPECT_TRUE(std::filesystem::exists(tmp));
    EXPECT_EQ(std::filesystem::file_size(tmp), 0u);
    std::filesystem::remove(tmp);
}

TEST(ChunkerTest, HashDifferentData) {
    std::vector<uint8_t> d1 = {'H', 'e', 'l', 'l', 'o'};
    std::vector<uint8_t> d2 = {'H', 'e', 'l', 'l', '!'};
    EXPECT_NE(hash_data(d1), hash_data(d2));
}

TEST(FastCDCTest, BufferChunkingReconstruction) {
    // Generate a 1 MB test payload
    std::vector<uint8_t> data(1024 * 1024);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 31 + 17) & 0xFF);
    }

    FastCDCConfig cfg{
        .min_size = 16 * 1024,
        .avg_size = 64 * 1024,
        .max_size = 128 * 1024
    };
    FastCDC cdc(cfg);
    auto chunks = cdc.chunk_buffer(data);

    EXPECT_FALSE(chunks.empty());

    uint64_t total_len = 0;
    for (size_t i = 0; i < chunks.size(); ++i) {
        const auto& c = chunks[i];
        if (i + 1 < chunks.size()) {
            EXPECT_GE(c.length, cfg.min_size);
        }
        EXPECT_LE(c.length, cfg.max_size);
        EXPECT_EQ(c.offset, total_len);
        EXPECT_FALSE(c.hash.empty());
        total_len += c.length;
    }
    EXPECT_EQ(total_len, data.size());
}

TEST(FastCDCTest, ContentDefinedBoundaryStability) {
    // Generate 1 MB of non-periodic pseudo-random data
    std::mt19937_64 rng(1337);
    std::vector<uint8_t> base_data(1024 * 1024);
    for (size_t i = 0; i < base_data.size(); i += 8) {
        uint64_t val = rng();
        std::memcpy(base_data.data() + i, &val, sizeof(val));
    }

    FastCDCConfig cfg{
        .min_size = 4 * 1024,
        .avg_size = 16 * 1024,
        .max_size = 32 * 1024
    };
    FastCDC cdc(cfg);
    auto base_chunks = cdc.chunk_buffer(base_data);

    // Create modified data with 256 bytes prepended
    std::vector<uint8_t> modified_data(256, 0xAA);
    modified_data.insert(modified_data.end(), base_data.begin(), base_data.end());

    auto mod_chunks = cdc.chunk_buffer(modified_data);

    // There should be matching chunk hashes across the two sets despite the shifted offset
    size_t matching_hashes = 0;
    for (const auto& bc : base_chunks) {
        for (const auto& mc : mod_chunks) {
            if (bc.hash == mc.hash) {
                matching_hashes++;
                break;
            }
        }
    }

    // A significant portion of chunks must have identical content hashes
    EXPECT_GT(matching_hashes, 0u);
}

