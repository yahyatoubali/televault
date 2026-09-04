#pragma once

#include <vector>
#include <cstdint>
#include <span>
#include <array>

namespace tv {

class StreamingEncryptor {
public:
    explicit StreamingEncryptor(std::span<const uint8_t> key);
    StreamingEncryptor(std::span<const uint8_t> key, std::span<const uint8_t> base_nonce);
    std::vector<uint8_t> process(std::span<const uint8_t> block);
    std::vector<uint8_t> finalize();

    [[nodiscard]] const std::array<uint8_t, 12>& base_nonce() const noexcept { return base_nonce_; }
    void set_base_nonce(std::span<const uint8_t> base_nonce);
    [[nodiscard]] uint32_t counter() const noexcept { return counter_; }
    void reset_counter() noexcept { counter_ = 0; }

private:
    std::array<uint8_t, 32> key_{};
    std::array<uint8_t, 12> base_nonce_{};
    uint32_t counter_{0};
};

class StreamingDecryptor {
public:
    explicit StreamingDecryptor(std::span<const uint8_t> key);
    StreamingDecryptor(std::span<const uint8_t> key, std::span<const uint8_t> base_nonce);
    std::vector<uint8_t> process(std::span<const uint8_t> block);
    std::vector<uint8_t> finalize();

    [[nodiscard]] const std::array<uint8_t, 12>& base_nonce() const noexcept { return base_nonce_; }
    void set_base_nonce(std::span<const uint8_t> base_nonce);
    [[nodiscard]] uint32_t counter() const noexcept { return counter_; }
    void reset_counter() noexcept { counter_ = 0; }

private:
    std::array<uint8_t, 32> key_{};
    std::array<uint8_t, 12> base_nonce_{};
    uint32_t counter_{0};
};

} // namespace tv
