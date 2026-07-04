#include "zstd.hpp"
#include <zstd.h>
#include <zstd_errors.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <span>
#include <ranges>

namespace tv {

static constexpr std::array incompressible_exts = {
    ".zip", ".rar", ".7z", ".gz", ".bz2", ".xz", ".zst",
    ".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp",
    ".mp4", ".mkv", ".avi", ".mov", ".webm",
    ".mp3", ".m4a", ".ogg", ".opus", ".flac", ".wav",
    ".pdf", ".docx", ".xlsx", ".pptx",
    ".webp",
};

bool should_compress(std::string_view filename) {
    auto pos = filename.rfind('.');
    if (pos == std::string_view::npos) return true;

    auto ext = filename.substr(pos);
    // Lowercase
    std::string lower_ext;
    lower_ext.resize(ext.size());
    std::transform(ext.begin(), ext.end(), lower_ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

    return std::ranges::none_of(incompressible_exts, [&](const char* ie) {
        return lower_ext == ie;
    });
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
    auto decompressed_size = ZSTD_getFrameContentSize(data.data(), data.size());
    if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN ||
        decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
        throw std::runtime_error("Cannot determine decompressed size");
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

uint64_t estimate_compressed_size(uint64_t uncompressed_size) {
    return ZSTD_compressBound(uncompressed_size);
}

} // namespace tv
