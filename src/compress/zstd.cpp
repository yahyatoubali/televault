#include "zstd.hpp"
#include "stream.hpp"
#include <zstd.h>
#include <zstd_errors.h>
#include <spdlog/spdlog.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <span>
#include <ranges>
#include <fstream>
#include <stdexcept>
#include <string_view>

using namespace std::string_view_literals;

namespace tv {

static constexpr auto incompressible_exts = std::to_array<std::string_view>({
    ".7z"sv,
    ".aac"sv,
    ".avi"sv,
    ".avif"sv,
    ".br"sv,
    ".bz2"sv,
    ".docx"sv,
    ".flac"sv,
    ".flv"sv,
    ".gif"sv,
    ".gz"sv,
    ".heic"sv,
    ".heif"sv,
    ".jpeg"sv,
    ".jpg"sv,
    ".lz4"sv,
    ".lzma"sv,
    ".m4a"sv,
    ".m4v"sv,
    ".mkv"sv,
    ".mov"sv,
    ".mp3"sv,
    ".mp4"sv,
    ".odt"sv,
    ".ogg"sv,
    ".opus"sv,
    ".pdf"sv,
    ".png"sv,
    ".pptx"sv,
    ".rar"sv,
    ".tar.gz"sv,
    ".tgz"sv,
    ".webm"sv,
    ".webp"sv,
    ".wma"sv,
    ".woff"sv,
    ".woff2"sv,
    ".xlsx"sv,
    ".xz"sv,
    ".zip"sv,
    ".zst"sv,
});

bool should_compress(std::string_view filename) {
    auto last_slash = filename.find_last_of("/\\");
    std::string_view base = (last_slash == std::string_view::npos)
                                ? filename
                                : filename.substr(last_slash + 1);

    auto pos = base.rfind('.');
    if (pos == std::string_view::npos) return true;

    auto ext = base.substr(pos);
    std::string lower_ext;
    lower_ext.reserve(ext.size());
    for (char c : ext) {
        lower_ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    return !std::ranges::binary_search(incompressible_exts, lower_ext);
}

std::vector<uint8_t> compress_data(std::span<const uint8_t> data, int level) {
    auto bound = ZSTD_compressBound(data.size());
    std::vector<uint8_t> compressed(bound);

    auto size = ZSTD_compress(compressed.data(), bound,
                              data.data(), data.size(), level);
    if (ZSTD_isError(size)) {
        throw std::runtime_error(std::string("Compression failed: ") +
                                 ZSTD_getErrorName(size));
    }

    compressed.resize(size);
    return compressed;
}

std::vector<uint8_t> decompress_data(std::span<const uint8_t> data) {
    if (data.empty()) {
        return {};
    }

    auto decompressed_size = ZSTD_getFrameContentSize(data.data(), data.size());
    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
        throw std::runtime_error("Invalid or corrupted zstd frame");
    }

    if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        StreamingDecompressor decompressor;
        auto result = decompressor.process(data);
        auto final_bytes = decompressor.finalize();
        result.insert(result.end(), final_bytes.begin(), final_bytes.end());
        return result;
    }

    std::vector<uint8_t> decompressed(decompressed_size);
    auto size = ZSTD_decompress(decompressed.data(), decompressed.size(),
                                data.data(), data.size());
    if (ZSTD_isError(size)) {
        throw std::runtime_error(std::string("Decompression failed: ") +
                                 ZSTD_getErrorName(size));
    }

    decompressed.resize(size);
    return decompressed;
}

bool is_zstd_frame(std::span<const uint8_t> data) noexcept {
    if (data.size() < 4) return false;
    return ZSTD_getFrameContentSize(data.data(), data.size()) != ZSTD_CONTENTSIZE_ERROR;
}

std::vector<uint8_t> decompress_data_tolerant(std::span<const uint8_t> data,
                                              bool compressed_flag) {
    if (!compressed_flag || data.empty()) {
        return {data.begin(), data.end()};
    }
    if (!is_zstd_frame(data)) {
        // Legacy vault files (e.g. .mp4 pushed before the bypass-flag fix)
        // were stored raw but marked compressed=true. Return bytes as-is.
        spdlog::warn("Chunk marked compressed but missing zstd frame magic; "
                     "treating {} bytes as stored raw", data.size());
        return {data.begin(), data.end()};
    }
    return decompress_data(data);
}

uint64_t estimate_compressed_size(uint64_t original_size, std::string_view filename) {
    if (!filename.empty() && !should_compress(filename)) {
        return original_size;
    }

    auto last_slash = filename.find_last_of("/\\");
    std::string_view base = (last_slash == std::string_view::npos)
                                ? filename
                                : filename.substr(last_slash + 1);
    auto pos = base.rfind('.');
    if (pos != std::string_view::npos) {
        auto ext = base.substr(pos);
        std::string lower_ext;
        lower_ext.reserve(ext.size());
        for (char c : ext) {
            lower_ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }

        static constexpr auto text_exts = std::to_array<std::string_view>({
            ".csv"sv, ".html"sv, ".json"sv, ".log"sv, ".md"sv, ".txt"sv, ".xml"sv
        });
        if (std::ranges::binary_search(text_exts, lower_ext)) {
            return static_cast<uint64_t>(original_size * 0.20);
        }

        static constexpr auto code_exts = std::to_array<std::string_view>({
            ".c"sv, ".cpp"sv, ".go"sv, ".h"sv, ".js"sv, ".py"sv, ".rs"sv, ".sql"sv, ".ts"sv
        });
        if (std::ranges::binary_search(code_exts, lower_ext)) {
            return static_cast<uint64_t>(original_size * 0.25);
        }

        static constexpr auto container_exts = std::to_array<std::string_view>({
            ".img"sv, ".iso"sv, ".tar"sv
        });
        if (std::ranges::binary_search(container_exts, lower_ext)) {
            return static_cast<uint64_t>(original_size * 0.60);
        }
    }

    return static_cast<uint64_t>(original_size * 0.50);
}

uint64_t compress_bound(uint64_t uncompressed_size) {
    return ZSTD_compressBound(uncompressed_size);
}

double compress_file(const std::filesystem::path& input_path,
                     const std::filesystem::path& output_path,
                     int level) {
    std::ifstream fin(input_path, std::ios::binary);
    if (!fin) {
        throw std::runtime_error("Failed to open input file: " + input_path.string());
    }
    std::ofstream fout(output_path, std::ios::binary);
    if (!fout) {
        throw std::runtime_error("Failed to open output file: " + output_path.string());
    }

    StreamingCompressor compressor(level);
    std::vector<uint8_t> buffer(64 * 1024);

    while (fin.read(reinterpret_cast<char*>(buffer.data()), buffer.size()) || fin.gcount() > 0) {
        auto count = static_cast<size_t>(fin.gcount());
        auto compressed = compressor.process(std::span<const uint8_t>(buffer.data(), count));
        if (!compressed.empty()) {
            fout.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
        }
    }

    auto final_chunk = compressor.finalize();
    if (!final_chunk.empty()) {
        fout.write(reinterpret_cast<const char*>(final_chunk.data()), final_chunk.size());
    }

    fout.flush();

    auto original_size = std::filesystem::file_size(input_path);
    auto compressed_size = std::filesystem::file_size(output_path);
    return original_size > 0 ? static_cast<double>(compressed_size) / static_cast<double>(original_size) : 1.0;
}

void decompress_file(const std::filesystem::path& input_path,
                     const std::filesystem::path& output_path) {
    std::ifstream fin(input_path, std::ios::binary);
    if (!fin) {
        throw std::runtime_error("Failed to open input file: " + input_path.string());
    }
    std::ofstream fout(output_path, std::ios::binary);
    if (!fout) {
        throw std::runtime_error("Failed to open output file: " + output_path.string());
    }

    StreamingDecompressor decompressor;
    std::vector<uint8_t> buffer(64 * 1024);

    while (fin.read(reinterpret_cast<char*>(buffer.data()), buffer.size()) || fin.gcount() > 0) {
        auto count = static_cast<size_t>(fin.gcount());
        auto decompressed = decompressor.process(std::span<const uint8_t>(buffer.data(), count));
        if (!decompressed.empty()) {
            fout.write(reinterpret_cast<const char*>(decompressed.data()), decompressed.size());
        }
    }

    auto final_chunk = decompressor.finalize();
    if (!final_chunk.empty()) {
        fout.write(reinterpret_cast<const char*>(final_chunk.data()), final_chunk.size());
    }

    fout.flush();
}

} // namespace tv
