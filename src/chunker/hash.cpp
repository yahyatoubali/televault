#include "hash.hpp"
#include <blake3.h>
#include <fstream>
#include <vector>
#include <array>
#include <span>
#include <iomanip>
#include <sstream>
#include <cstdint>

namespace tv {

static constexpr size_t HASH_HEX_SIZE = 64; // BLAKE3 is 32 bytes → 64 hex chars

std::string hash_to_hex(std::span<const uint8_t, 32> hash) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : hash) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

std::string hash_data(std::span<const uint8_t> data) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data.data(), data.size());

    std::array<uint8_t, 32> hash{};
    blake3_hasher_finalize(&hasher, hash.data(), hash.size());

    return hash_to_hex(hash);
}

std::string hash_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file for hashing: " + path);
    }

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);

    std::array<char, 65536> buf{};
    while (true) {
        file.read(buf.data(), buf.size());
        auto count = file.gcount();
        if (count > 0) {
            blake3_hasher_update(&hasher, buf.data(), static_cast<size_t>(count));
        }
        if (file.fail() && !file.eof()) {
            throw std::runtime_error("Read error while hashing: " + path);
        }
        if (count < static_cast<std::streamsize>(buf.size())) break;
    }

    std::array<uint8_t, 32> hash{};
    blake3_hasher_finalize(&hasher, hash.data(), hash.size());

    return hash_to_hex(hash);
}

std::string hash_data_async(std::span<const uint8_t> data) {
    return hash_data(data);
}

std::string hash_file_async(const std::string& path) {
    return hash_file(path);
}

} // namespace tv
