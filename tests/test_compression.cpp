#include <gtest/gtest.h>
#include <vector>
#include <span>
#include <cstdint>
#include <string>

#include "compress/zstd.hpp"
#include "compress/stream.hpp"

using namespace tv;

TEST(CompressionTest, CompressDecompressRoundTrip) {
    std::string original = "Hello, TeleVault! This is test data that should compress well. "
                           "Repeating patterns make compression efficient. "
                           "Hello, TeleVault! This is test data that should compress well. ";
    std::vector<uint8_t> data(original.begin(), original.end());

    auto compressed = compress_data(data);
    EXPECT_LE(compressed.size(), data.size()); // should compress somewhat

    auto decompressed = decompress_data(compressed);
    EXPECT_EQ(decompressed.size(), data.size());
    EXPECT_EQ(
        std::vector<uint8_t>(data.begin(), data.end()),
        decompressed
    );
}

TEST(CompressionTest, CompressEmptyData) {
    std::vector<uint8_t> data;
    auto compressed = compress_data(data);
    auto decompressed = decompress_data(compressed);
    EXPECT_TRUE(decompressed.empty());
}

TEST(CompressionTest, ShouldCompress) {
    EXPECT_TRUE(should_compress("document.txt"));
    EXPECT_TRUE(should_compress("script.py"));
    EXPECT_TRUE(should_compress("data.json"));
    EXPECT_FALSE(should_compress("image.jpg"));
    EXPECT_FALSE(should_compress("archive.zip"));
    EXPECT_FALSE(should_compress("video.mp4"));
    EXPECT_FALSE(should_compress("music.mp3"));
}

TEST(CompressionTest, CompressionLevels) {
    std::vector<uint8_t> data(100000, 'A'); // highly compressible

    auto fast = compress_data(data, 1);
    auto normal = compress_data(data, 3);
    auto max_comp = compress_data(data, 19);

    EXPECT_LE(fast.size(), data.size());
    EXPECT_LE(normal.size(), fast.size()); // higher level = smaller
    EXPECT_LE(max_comp.size(), normal.size());
}

TEST(CompressionTest, StreamingCompression) {
    std::vector<uint8_t> data(100000, 'B');
    StreamingCompressor comp(3);

    std::vector<uint8_t> compressed;
    size_t offset = 0;
    while (offset < data.size()) {
        auto chunk_size = std::min<size_t>(16384, data.size() - offset);
        auto out = comp.process({data.data() + offset, chunk_size});
        compressed.insert(compressed.end(), out.begin(), out.end());
        offset += chunk_size;
    }
    auto final = comp.finalize();
    compressed.insert(compressed.end(), final.begin(), final.end());

    StreamingDecompressor decomp;
    std::vector<uint8_t> decompressed;
    offset = 0;
    while (offset < compressed.size()) {
        auto chunk_size = std::min<size_t>(16384, compressed.size() - offset);
        auto out = decomp.process({compressed.data() + offset, chunk_size});
        decompressed.insert(decompressed.end(), out.begin(), out.end());
        offset += chunk_size;
    }

    EXPECT_EQ(decompressed, data);
}

TEST(CompressionTest, EstimateCompressedSize) {
    auto bound = estimate_compressed_size(1000);
    EXPECT_GE(bound, 1000);
}
