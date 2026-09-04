#pragma once

#include <vector>
#include <cstdint>
#include <span>
#include <memory>

namespace tv {

class StreamingCompressor {
public:
    explicit StreamingCompressor(int level = 3);
    ~StreamingCompressor();

    StreamingCompressor(StreamingCompressor&&) noexcept;
    StreamingCompressor& operator=(StreamingCompressor&&) noexcept;

    StreamingCompressor(const StreamingCompressor&) = delete;
    StreamingCompressor& operator=(const StreamingCompressor&) = delete;

    std::vector<uint8_t> process(std::span<const uint8_t> data);
    std::vector<uint8_t> finalize();

    std::vector<uint8_t> compress(std::span<const uint8_t> data) { return process(data); }
    std::vector<uint8_t> flush();
    [[nodiscard]] double ratio() const noexcept;
    [[nodiscard]] uint64_t total_in() const noexcept;
    [[nodiscard]] uint64_t total_out() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class StreamingDecompressor {
public:
    StreamingDecompressor();
    ~StreamingDecompressor();

    StreamingDecompressor(StreamingDecompressor&&) noexcept;
    StreamingDecompressor& operator=(StreamingDecompressor&&) noexcept;

    StreamingDecompressor(const StreamingDecompressor&) = delete;
    StreamingDecompressor& operator=(const StreamingDecompressor&) = delete;

    std::vector<uint8_t> process(std::span<const uint8_t> data);
    std::vector<uint8_t> finalize();

    std::vector<uint8_t> decompress(std::span<const uint8_t> data) { return process(data); }
    std::vector<uint8_t> flush() { return finalize(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv
