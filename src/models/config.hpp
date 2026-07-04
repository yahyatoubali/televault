#pragma once

#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace tv {

struct RetryConfig {
    int max_retries{5};
    int base_delay_ms{1000};
    int max_delay_ms{300000};
    double jitter_factor{0.1};
};

struct LowResourceConfig {
    bool enabled{};
    uint64_t chunk_size{32 * 1024 * 1024};
    int parallel_uploads{2};
    int parallel_downloads{2};
    int hasher_threads{1};
    bool sequential_download{true};
    uint64_t max_no_compress_size{500 * 1024 * 1024};
};

struct TelegramConfig {
    int32_t api_id{};
    std::string api_hash;
    std::string phone;
};

struct Config {
    int64_t channel_id{};
    int64_t index_msg_id{};
    uint64_t chunk_size{256 * 1024 * 1024};
    bool compression{true};
    bool encryption{true};
    int parallel_uploads{8};
    int parallel_downloads{10};
    RetryConfig retry;
    LowResourceConfig low_resource;
    TelegramConfig telegram;
    std::string config_dir;
    std::string data_dir;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RetryConfig, max_retries, base_delay_ms, max_delay_ms, jitter_factor)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LowResourceConfig, enabled, chunk_size, parallel_uploads, parallel_downloads, hasher_threads, sequential_download, max_no_compress_size)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TelegramConfig, api_id, api_hash, phone)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Config, channel_id, index_msg_id, chunk_size, compression, encryption, parallel_uploads, parallel_downloads, retry, low_resource, telegram)

} // namespace tv
