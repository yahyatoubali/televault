#include "stream.hpp"
#include <zstd.h>
#include <zstd_errors.h>
#include <stdexcept>
#include <cstring>
#include <memory>

namespace tv {

namespace {

struct CStreamDeleter {
    void operator()(ZSTD_CStream* s) const noexcept {
        if (s) ZSTD_freeCStream(s);
    }
};

struct DStreamDeleter {
    void operator()(ZSTD_DStream* s) const noexcept {
        if (s) ZSTD_freeDStream(s);
    }
};

using CStreamPtr = std::unique_ptr<ZSTD_CStream, CStreamDeleter>;
using DStreamPtr = std::unique_ptr<ZSTD_DStream, DStreamDeleter>;

} // namespace

// ── StreamingCompressor ────────────────────────────────────────────

struct StreamingCompressor::Impl {
    CStreamPtr cstream;
    uint64_t total_in{0};
    uint64_t total_out{0};
    bool finalized{false};

    explicit Impl(int level)
        : cstream(ZSTD_createCStream()) {
        if (!cstream) {
            throw std::runtime_error("Failed to create Zstandard CStream context");
        }
        auto err = ZSTD_CCtx_setParameter(cstream.get(), ZSTD_c_compressionLevel, level);
        if (ZSTD_isError(err)) {
            throw std::runtime_error(std::string("Failed to set compression level: ") +
                                     ZSTD_getErrorName(err));
        }
    }
};

StreamingCompressor::StreamingCompressor(int level)
    : impl_(std::make_unique<Impl>(level)) {}

StreamingCompressor::~StreamingCompressor() = default;
StreamingCompressor::StreamingCompressor(StreamingCompressor&&) noexcept = default;
StreamingCompressor& StreamingCompressor::operator=(StreamingCompressor&&) noexcept = default;

std::vector<uint8_t> StreamingCompressor::process(std::span<const uint8_t> data) {
    if (impl_->finalized) {
        throw std::runtime_error("StreamingCompressor has already been finalized");
    }
    if (data.empty()) {
        return {};
    }

    impl_->total_in += data.size();
    ZSTD_inBuffer input{data.data(), data.size(), 0};
    std::vector<uint8_t> result;

    while (input.pos < input.size) {
        std::vector<uint8_t> output(ZSTD_CStreamOutSize());
        ZSTD_outBuffer out{output.data(), output.size(), 0};

        auto remaining = ZSTD_compressStream2(impl_->cstream.get(), &out, &input, ZSTD_e_continue);
        if (ZSTD_isError(remaining)) {
            throw std::runtime_error(std::string("Streaming compression failed: ") +
                                     ZSTD_getErrorName(remaining));
        }

        output.resize(out.pos);
        result.insert(result.end(), output.begin(), output.end());
    }

    impl_->total_out += result.size();
    return result;
}

std::vector<uint8_t> StreamingCompressor::flush() {
    if (impl_->finalized) {
        return {};
    }
    std::vector<uint8_t> result;
    ZSTD_inBuffer input{nullptr, 0, 0};

    while (true) {
        std::vector<uint8_t> output(ZSTD_CStreamOutSize());
        ZSTD_outBuffer out{output.data(), output.size(), 0};

        auto remaining = ZSTD_compressStream2(impl_->cstream.get(), &out, &input, ZSTD_e_flush);
        if (ZSTD_isError(remaining)) {
            throw std::runtime_error(std::string("Streaming compression flush failed: ") +
                                     ZSTD_getErrorName(remaining));
        }

        output.resize(out.pos);
        result.insert(result.end(), output.begin(), output.end());
        if (remaining == 0) {
            break;
        }
    }

    impl_->total_out += result.size();
    return result;
}

std::vector<uint8_t> StreamingCompressor::finalize() {
    if (impl_->finalized) {
        return {};
    }
    impl_->finalized = true;

    std::vector<uint8_t> result;
    ZSTD_inBuffer input{nullptr, 0, 0};

    while (true) {
        std::vector<uint8_t> output(ZSTD_CStreamOutSize());
        ZSTD_outBuffer out{output.data(), output.size(), 0};

        auto remaining = ZSTD_compressStream2(impl_->cstream.get(), &out, &input, ZSTD_e_end);
        if (ZSTD_isError(remaining)) {
            throw std::runtime_error(std::string("Streaming compression finalize failed: ") +
                                     ZSTD_getErrorName(remaining));
        }

        output.resize(out.pos);
        result.insert(result.end(), output.begin(), output.end());
        if (remaining == 0) {
            break;
        }
    }

    impl_->total_out += result.size();
    return result;
}

double StreamingCompressor::ratio() const noexcept {
    if (impl_->total_in == 0) return 1.0;
    return static_cast<double>(impl_->total_out) / static_cast<double>(impl_->total_in);
}

uint64_t StreamingCompressor::total_in() const noexcept {
    return impl_->total_in;
}

uint64_t StreamingCompressor::total_out() const noexcept {
    return impl_->total_out;
}

// ── StreamingDecompressor ──────────────────────────────────────────

struct StreamingDecompressor::Impl {
    DStreamPtr dstream;
    bool finalized{false};

    Impl() : dstream(ZSTD_createDStream()) {
        if (!dstream) {
            throw std::runtime_error("Failed to create Zstandard DStream context");
        }
        auto err = ZSTD_initDStream(dstream.get());
        if (ZSTD_isError(err)) {
            throw std::runtime_error(std::string("Failed to init Zstandard DStream: ") +
                                     ZSTD_getErrorName(err));
        }
    }
};

StreamingDecompressor::StreamingDecompressor()
    : impl_(std::make_unique<Impl>()) {}

StreamingDecompressor::~StreamingDecompressor() = default;
StreamingDecompressor::StreamingDecompressor(StreamingDecompressor&&) noexcept = default;
StreamingDecompressor& StreamingDecompressor::operator=(StreamingDecompressor&&) noexcept = default;

std::vector<uint8_t> StreamingDecompressor::process(std::span<const uint8_t> data) {
    if (data.empty()) {
        return {};
    }

    ZSTD_inBuffer input{data.data(), data.size(), 0};
    std::vector<uint8_t> result;

    while (input.pos < input.size) {
        std::vector<uint8_t> output(ZSTD_DStreamOutSize());
        ZSTD_outBuffer out{output.data(), output.size(), 0};

        auto ret = ZSTD_decompressStream(impl_->dstream.get(), &out, &input);
        if (ZSTD_isError(ret)) {
            throw std::runtime_error(std::string("Streaming decompression failed: ") +
                                     ZSTD_getErrorName(ret));
        }

        output.resize(out.pos);
        result.insert(result.end(), output.begin(), output.end());

        while (out.pos == out.size && ret > 0) {
            std::vector<uint8_t> drain_buf(ZSTD_DStreamOutSize());
            ZSTD_outBuffer drain_out{drain_buf.data(), drain_buf.size(), 0};
            ret = ZSTD_decompressStream(impl_->dstream.get(), &drain_out, &input);
            if (ZSTD_isError(ret)) {
                throw std::runtime_error(std::string("Streaming decompression failed: ") +
                                         ZSTD_getErrorName(ret));
            }
            drain_buf.resize(drain_out.pos);
            result.insert(result.end(), drain_buf.begin(), drain_buf.end());
            if (drain_out.pos < drain_out.size) {
                break;
            }
        }
    }

    return result;
}

std::vector<uint8_t> StreamingDecompressor::finalize() {
    if (impl_->finalized) {
        return {};
    }
    impl_->finalized = true;

    std::vector<uint8_t> result;
    ZSTD_inBuffer input{nullptr, 0, 0};

    while (true) {
        std::vector<uint8_t> output(ZSTD_DStreamOutSize());
        ZSTD_outBuffer out{output.data(), output.size(), 0};

        auto ret = ZSTD_decompressStream(impl_->dstream.get(), &out, &input);
        if (ZSTD_isError(ret)) {
            throw std::runtime_error(std::string("Streaming decompression finalize failed: ") +
                                     ZSTD_getErrorName(ret));
        }

        if (out.pos == 0) {
            break;
        }

        output.resize(out.pos);
        result.insert(result.end(), output.begin(), output.end());

        if (out.pos < out.size) {
            break;
        }
    }

    return result;
}

} // namespace tv
