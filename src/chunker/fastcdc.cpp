#include "fastcdc.hpp"
#include "hash.hpp"
#include <algorithm>
#include <fstream>
#include <bit>

namespace tv {

// Canonical 64-bit gear matrix generated via SplitMix64 for optimal entropy across all bit positions
static constexpr auto generate_gear_matrix() {
    std::array<uint64_t, 256> table{};
    uint64_t x = 0x9E3779B97F4A7C15ULL;
    for (size_t i = 0; i < 256; ++i) {
        uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        table[i] = z ^ (z >> 31);
    }
    return table;
}

static constexpr auto GEAR_MATRIX = generate_gear_matrix();

static uint64_t compute_mask(uint32_t bits) {
    if (bits == 0) return 0;
    if (bits >= 64) return ~0ULL;
    return (1ULL << bits) - 1;
}

FastCDC::FastCDC(FastCDCConfig config) : config_(config) {
    init_masks();
}

void FastCDC::init_masks() {
    // Determine number of mask bits based on average chunk size: 2^bits ≈ avg_size
    uint32_t bits = 0;
    uint64_t avg = config_.avg_size;
    while (avg > 1) {
        avg >>= 1;
        bits++;
    }

    // FastCDC uses two normalized masks: mask_s has more bits to avoid small chunks,
    // mask_l has fewer bits to encourage cuts before reaching max_size.
    uint32_t bits_s = bits + 1;
    uint32_t bits_l = (bits > 1) ? bits - 1 : 1;

    mask_s_ = compute_mask(bits_s);
    mask_l_ = compute_mask(bits_l);
}

uint64_t FastCDC::find_cut_point(std::span<const uint8_t> data) const {
    const uint64_t len = data.size();
    if (len <= config_.min_size) {
        return len;
    }

    uint64_t fingerprint = 0;
    uint64_t pos = config_.min_size;
    const uint64_t max_cut = std::min(len, config_.max_size);
    const uint64_t mid_cut = std::min(max_cut, config_.avg_size);

    // Phase 1: Small mask check
    while (pos < mid_cut) {
        fingerprint = (fingerprint << 1) + GEAR_MATRIX[data[pos]];
        if ((fingerprint & mask_s_) == 0) {
            return pos + 1;
        }
        pos++;
    }

    // Phase 2: Large mask check up to max_cut
    while (pos < max_cut) {
        fingerprint = (fingerprint << 1) + GEAR_MATRIX[data[pos]];
        if ((fingerprint & mask_l_) == 0) {
            return pos + 1;
        }
        pos++;
    }

    return max_cut;
}

std::vector<FastCDCChunk> FastCDC::chunk_buffer(std::span<const uint8_t> data) const {
    std::vector<FastCDCChunk> chunks;
    uint64_t offset = 0;

    while (offset < data.size()) {
        auto remaining = data.subspan(offset);
        uint64_t cut = find_cut_point(remaining);

        FastCDCChunk chunk;
        chunk.offset = offset;
        chunk.length = cut;
        chunk.hash = hash_data(remaining.subspan(0, cut));

        chunks.push_back(std::move(chunk));
        offset += cut;
    }

    return chunks;
}

bool FastCDC::chunk_file(const std::string& path,
                         std::function<bool(const FastCDCChunk&, std::span<const uint8_t>)> callback) const {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    // Buffer up to 2 * max_size to maintain continuous FastCDC rolling hash across read boundaries
    const size_t buffer_cap = static_cast<size_t>(config_.max_size * 2);
    std::vector<uint8_t> buffer(buffer_cap);
    size_t valid_bytes = 0;
    uint64_t file_offset = 0;

    while (file || valid_bytes > 0) {
        if (valid_bytes < config_.max_size && file) {
            file.read(reinterpret_cast<char*>(buffer.data() + valid_bytes),
                      static_cast<std::streamsize>(buffer_cap - valid_bytes));
            valid_bytes += static_cast<size_t>(file.gcount());
        }

        if (valid_bytes == 0) break;

        uint64_t cut = find_cut_point(std::span<const uint8_t>(buffer.data(), valid_bytes));
        auto chunk_span = std::span<const uint8_t>(buffer.data(), cut);

        FastCDCChunk c;
        c.offset = file_offset;
        c.length = cut;
        c.hash = hash_data(chunk_span);

        if (!callback(c, chunk_span)) {
            return false;
        }

        file_offset += cut;
        size_t remaining_bytes = valid_bytes - cut;
        if (remaining_bytes > 0) {
            std::copy(buffer.data() + cut, buffer.data() + valid_bytes, buffer.data());
        }
        valid_bytes = remaining_bytes;
    }

    return true;
}

} // namespace tv
