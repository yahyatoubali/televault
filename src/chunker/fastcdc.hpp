#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>
#include <string>
#include <functional>

namespace tv {

struct FastCDCConfig {
    uint64_t min_size{4 * 1024 * 1024};   // 4 MB minimum chunk
    uint64_t avg_size{16 * 1024 * 1024};  // 16 MB target average chunk
    uint64_t max_size{32 * 1024 * 1024};  // 32 MB maximum chunk

    static FastCDCConfig default_config() {
        return {4 * 1024 * 1024, 16 * 1024 * 1024, 32 * 1024 * 1024};
    }

    static FastCDCConfig low_resource() {
        return {1 * 1024 * 1024, 4 * 1024 * 1024, 8 * 1024 * 1024};
    }
};

struct FastCDCChunk {
    uint64_t offset{0};
    uint64_t length{0};
    std::string hash; // Blake3 hash of chunk content
};

class FastCDC {
public:
    explicit FastCDC(FastCDCConfig config = FastCDCConfig::default_config());

    /// Computes chunk boundaries for a memory buffer
    [[nodiscard]] std::vector<FastCDCChunk> chunk_buffer(std::span<const uint8_t> data) const;

    /// Streams through a file on disk, identifying FastCDC chunk boundaries and calling `callback`
    /// callback returns false to abort processing
    bool chunk_file(const std::string& path,
                    std::function<bool(const FastCDCChunk&, std::span<const uint8_t>)> callback) const;

    [[nodiscard]] const FastCDCConfig& config() const noexcept { return config_; }

private:
    FastCDCConfig config_;
    uint64_t mask_s_{0};
    uint64_t mask_l_{0};

    void init_masks();
    [[nodiscard]] uint64_t find_cut_point(std::span<const uint8_t> data) const;
};

} // namespace tv
