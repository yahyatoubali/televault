#include <gtest/gtest.h>
#include <vector>
#include <span>
#include <cstdint>
#include <string>
#include <filesystem>
#include <fstream>

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
    EXPECT_FALSE(should_compress("photo.heic"));
    EXPECT_FALSE(should_compress("archive.tar.gz"));
    EXPECT_FALSE(should_compress("archive.tgz"));
    EXPECT_FALSE(should_compress("archive.xz"));
    EXPECT_FALSE(should_compress("document.pdf"));
    EXPECT_FALSE(should_compress("word.docx"));
    EXPECT_TRUE(should_compress("uncompressed.bmp"));
    EXPECT_TRUE(should_compress("recording.wav"));
    EXPECT_TRUE(is_compressible("script.py"));
    EXPECT_FALSE(is_compressible("path/to/archive.zip"));
    EXPECT_FALSE(should_compress("IMAGE.JPG"));
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

TEST(CompressionTest, StreamingCompressorProperties) {
    StreamingCompressor comp(3);
    std::vector<uint8_t> data(20000, 'C');
    auto part1 = comp.compress(std::span<const uint8_t>(data.data(), 10000));
    auto flushed = comp.flush();
    auto part2 = comp.compress(std::span<const uint8_t>(data.data() + 10000, 10000));
    auto finalized = comp.finalize();

    EXPECT_EQ(comp.total_in(), 20000);
    EXPECT_GT(comp.total_out(), 0);
    EXPECT_LT(comp.ratio(), 0.1);
}

TEST(CompressionTest, DecompressUnknownContentSize) {
    StreamingCompressor comp(3);
    std::vector<uint8_t> data(50000, 'D');
    auto chunk = comp.compress(data);
    auto end = comp.finalize();
    chunk.insert(chunk.end(), end.begin(), end.end());

    auto decompressed = decompress_data(chunk);
    EXPECT_EQ(decompressed, data);
}

TEST(CompressionTest, EstimateCompressedSize) {
    EXPECT_EQ(estimate_compressed_size(1000, "file.txt"), 200);
    EXPECT_EQ(estimate_compressed_size(1000, "script.py"), 250);
    EXPECT_EQ(estimate_compressed_size(1000, "main.cpp"), 250);
    EXPECT_EQ(estimate_compressed_size(1000, "archive.tar"), 600);
    EXPECT_EQ(estimate_compressed_size(1000, "archive.zip"), 1000);
    EXPECT_EQ(estimate_compressed_size(1000, "unknown.bin"), 500);
    EXPECT_EQ(estimate_compressed_size(1000), 500);
    EXPECT_GE(compress_bound(1000), 1000);
}

TEST(CompressionTest, CompressDecompressFileRoundtrip) {    auto tmp_dir = std::filesystem::temp_directory_path();
    auto in_file = tmp_dir / "televault_test_in.txt";
    auto cmp_file = tmp_dir / "televault_test_cmp.zst";
    auto out_file = tmp_dir / "televault_test_out.txt";

    std::ofstream fout(in_file);
    for (int i = 0; i < 1000; ++i) {
        fout << "Line " << i << ": TeleVault compression file test payload repeated\n";
    }
    fout.close();

    double ratio = compress_file(in_file, cmp_file);
    EXPECT_LT(ratio, 0.25);

    decompress_file(cmp_file, out_file);

    std::ifstream f_orig(in_file, std::ios::binary);
    std::ifstream f_dec(out_file, std::ios::binary);
    std::string orig_str((std::istreambuf_iterator<char>(f_orig)), std::istreambuf_iterator<char>());
    std::string dec_str((std::istreambuf_iterator<char>(f_dec)), std::istreambuf_iterator<char>());
    EXPECT_EQ(orig_str, dec_str);

    std::filesystem::remove(in_file);
    std::filesystem::remove(cmp_file);
    std::filesystem::remove(out_file);
}

// Regression: `tvt stream` aborted with "Invalid or corrupted zstd frame"
// on .mp4 files. Push bypassed compression for incompressible media but
// recorded meta.compressed=true, so every read path tried to decompress
// raw bytes. The tolerant helper must pass raw bytes through.
TEST(CompressionTest, TolerantDecompressPassesRawMediaThrough) {
    // Vault-style mp4 name from the reported crash
    EXPECT_FALSE(should_compress("f677638a-1b48-4ced-b18f-7359cbbc12f1_1080p_mp4_30_16-9.mp4"));
    EXPECT_FALSE(should_compress("CLIP.MP4"));

    // ftyp magic like a real mp4 header — not a zstd frame
    std::vector<uint8_t> raw = {0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p',
                                'm', 'p', '4', '2'};
    EXPECT_FALSE(is_zstd_frame(raw));
    EXPECT_EQ(decompress_data_tolerant(raw, true), raw);
    EXPECT_EQ(decompress_data_tolerant(raw, false), raw);
    EXPECT_TRUE(decompress_data_tolerant(std::vector<uint8_t>{}, true).empty());

    // Genuine zstd payloads still round-trip through the tolerant path
    std::string txt(5000, 'a');
    std::vector<uint8_t> pt(txt.begin(), txt.end());
    auto ct = compress_data(pt);
    EXPECT_TRUE(is_zstd_frame(ct));
    EXPECT_EQ(decompress_data_tolerant(ct, true), pt);

    // Strict API behavior is unchanged (still throws on raw input)
    EXPECT_THROW(decompress_data(raw), std::runtime_error);
}
