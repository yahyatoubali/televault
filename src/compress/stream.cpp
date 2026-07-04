#include "stream.hpp"
#include <zstd.h>
#include <stdexcept>
#include <cstring>
#include <memory>

namespace tv {

class StreamingCompressor::Impl {
public:
    ZSTD_CCtx* cctx{};
    Impl(int level) {
        cctx = ZSTD_createCCtx();
        if (!cctx) throw std::runtime_error("Failed to create zstd compression context");
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
    }
    ~Impl() { if (cctx) ZSTD_freeCCtx(cctx); }
};

StreamingCompressor::StreamingCompressor(int level)
    : impl_(std::make_unique<Impl>(level)) {}

StreamingCompressor::~StreamingCompressor() = default;

std::vector<uint8_t> StreamingCompressor::process(std::span<const uint8_t> data) {
    ZSTD_inBuffer input{data.data(), data.size(), 0};
    std::vector<uint8_t> result;

    while (input.pos < input.size) {
        std::vector<uint8_t> output(ZSTD_CStreamOutSize());
        ZSTD_outBuffer out{output.data(), output.size(), 0};

        auto ret = ZSTD_compressStream2(impl_->cctx, &out, &input, ZSTD_e_continue);
        if (ZSTD_isError(ret)) {
            throw std::runtime_error(ZSTD_getErrorName(ret));
        }

        output.resize(out.pos);
        result.insert(result.end(), output.begin(), output.end());
    }
    return result;
}

std::vector<uint8_t> StreamingCompressor::finalize() {
    std::vector<uint8_t> result;
    ZSTD_inBuffer input{nullptr, 0, 0};

    while (true) {
        std::vector<uint8_t> output(ZSTD_CStreamOutSize());
        ZSTD_outBuffer out{output.data(), output.size(), 0};

        auto ret = ZSTD_compressStream2(impl_->cctx, &out, &input, ZSTD_e_end);
        if (ZSTD_isError(ret)) {
            throw std::runtime_error(ZSTD_getErrorName(ret));
        }

        output.resize(out.pos);
        result.insert(result.end(), output.begin(), output.end());
        if (ret == 0) break;
    }
    return result;
}

// ── StreamingDecompressor ──────────────────────────────────────────

class StreamingDecompressor::Impl {
public:
    ZSTD_DCtx* dctx{};
    Impl() {
        dctx = ZSTD_createDCtx();
        if (!dctx) throw std::runtime_error("Failed to create zstd decompression context");
    }
    ~Impl() { if (dctx) ZSTD_freeDCtx(dctx); }
};

StreamingDecompressor::StreamingDecompressor()
    : impl_(std::make_unique<Impl>()) {}

StreamingDecompressor::~StreamingDecompressor() = default;

std::vector<uint8_t> StreamingDecompressor::process(std::span<const uint8_t> data) {
    ZSTD_inBuffer input{data.data(), data.size(), 0};
    std::vector<uint8_t> result;

    while (input.pos < input.size) {
        std::vector<uint8_t> output(ZSTD_DStreamOutSize());
        ZSTD_outBuffer out{output.data(), output.size(), 0};

        auto ret = ZSTD_decompressStream(impl_->dctx, &out, &input);
        if (ZSTD_isError(ret)) {
            throw std::runtime_error(ZSTD_getErrorName(ret));
        }

        output.resize(out.pos);
        result.insert(result.end(), output.begin(), output.end());
        if (ret == 0 && input.pos >= input.size) break;
    }
    return result;
}

std::vector<uint8_t> StreamingDecompressor::finalize() {
    return {};
}

} // namespace tv
