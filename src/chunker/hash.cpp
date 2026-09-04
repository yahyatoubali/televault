#include "hash.hpp"
#include <blake3.h>
#include <fstream>
#include <vector>
#include <array>
#include <span>
#include <algorithm>
#include <stdexcept>

namespace tv {

std::string hash_to_hex(std::span<const uint8_t> hash, size_t prefix_len) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    size_t target_len = std::min(prefix_len, hash.size() * 2);
    std::string out;
    out.reserve(target_len);
    for (size_t i = 0; i < hash.size() && out.size() < target_len; ++i) {
        out.push_back(hex_chars[(hash[i] >> 4) & 0x0F]);
        if (out.size() < target_len) {
            out.push_back(hex_chars[hash[i] & 0x0F]);
        }
    }
    return out;
}

std::string hash_data(std::span<const uint8_t> data, size_t prefix_len) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    if (!data.empty()) {
        blake3_hasher_update(&hasher, data.data(), data.size());
    }

    std::array<uint8_t, 32> hash{};
    blake3_hasher_finalize(&hasher, hash.data(), hash.size());

    return hash_to_hex(hash, prefix_len);
}

std::string hash_data_prefix(std::span<const uint8_t> data, size_t prefix_len) {
    return hash_data(data, prefix_len);
}

std::string hash_file(const std::string& path, size_t prefix_len) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file for hashing: " + path);
    }

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);

    std::array<char, 65536> buf{};
    while (true) {
        file.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        auto count = file.gcount();
        if (count > 0) {
            blake3_hasher_update(&hasher, buf.data(), static_cast<size_t>(count));
        }
        if (file.fail() && !file.eof()) {
            throw std::runtime_error("Read error while hashing: " + path);
        }
        if (count < static_cast<std::streamsize>(buf.size())) {
            break;
        }
    }

    std::array<uint8_t, 32> hash{};
    blake3_hasher_finalize(&hasher, hash.data(), hash.size());

    return hash_to_hex(hash, prefix_len);
}

std::string hash_file_prefix(const std::string& path, size_t prefix_len) {
    return hash_file(path, prefix_len);
}

std::string hash_data_async(std::span<const uint8_t> data, size_t prefix_len) {
    return hash_data(data, prefix_len);
}

std::string hash_file_async(const std::string& path, size_t prefix_len) {
    return hash_file(path, prefix_len);
}

Blake3Hasher::Blake3Hasher() {
    reset();
}

void Blake3Hasher::reset() {
    blake3_hasher_init(&hasher_);
}

void Blake3Hasher::update(std::span<const uint8_t> data) {
    if (!data.empty()) {
        blake3_hasher_update(&hasher_, data.data(), data.size());
    }
}

void Blake3Hasher::update(const void* data, size_t size) {
    if (data != nullptr && size > 0) {
        blake3_hasher_update(&hasher_, data, size);
    }
}

std::string Blake3Hasher::finalize(size_t prefix_len) {
    std::array<uint8_t, 32> hash{};
    blake3_hasher_finalize(&hasher_, hash.data(), hash.size());
    return hash_to_hex(hash, prefix_len);
}

bool hash_matches(std::string_view computed, std::string_view expected) noexcept {
    if (expected.empty() || computed.empty()) {
        return false;
    }
    if (expected.size() == computed.size()) {
        return expected == computed;
    }
    size_t cmp_len = std::min(expected.size(), computed.size());
    return computed.substr(0, cmp_len) == expected.substr(0, cmp_len);
}

} // namespace tv
