#pragma once

#include <vector>
#include <cstdint>
#include <span>
#include <array>

namespace tv {

class StreamingEncryptor {
public:
    explicit StreamingEncryptor(std::span<const uint8_t> key);
    std::vector<uint8_t> process(std::span<const uint8_t> block);
    std::vector<uint8_t> finalize();
};

class StreamingDecryptor {
public:
    explicit StreamingDecryptor(std::span<const uint8_t> key);
    std::vector<uint8_t> process(std::span<const uint8_t> block);
    std::vector<uint8_t> finalize();
};

} // namespace tv
