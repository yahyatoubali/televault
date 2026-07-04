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
    std::vector<uint8_t> process(std::span<const uint8_t> data);
    std::vector<uint8_t> finalize();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class StreamingDecompressor {
public:
    StreamingDecompressor();
    ~StreamingDecompressor();
    std::vector<uint8_t> process(std::span<const uint8_t> data);
    std::vector<uint8_t> finalize();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv
