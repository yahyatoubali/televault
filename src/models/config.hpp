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
    double retry_delay{1.0};
};

inline void to_json(nlohmann::json& j, const RetryConfig& r) {
    j = nlohmann::json{
        {"max_retries", r.max_retries},
        {"base_delay_ms", r.base_delay_ms},
        {"max_delay_ms", r.max_delay_ms},
        {"jitter_factor", r.jitter_factor},
        {"retry_delay", r.retry_delay}
    };
}

inline void from_json(const nlohmann::json& j, RetryConfig& r) {
    if (j.contains("max_retries") && !j["max_retries"].is_null()) r.max_retries = j["max_retries"].get<int>();
    if (j.contains("base_delay_ms") && !j["base_delay_ms"].is_null()) r.base_delay_ms = j["base_delay_ms"].get<int>();
    if (j.contains("max_delay_ms") && !j["max_delay_ms"].is_null()) r.max_delay_ms = j["max_delay_ms"].get<int>();
    if (j.contains("jitter_factor") && !j["jitter_factor"].is_null()) r.jitter_factor = j["jitter_factor"].get<double>();
    if (j.contains("retry_delay") && !j["retry_delay"].is_null()) r.retry_delay = j["retry_delay"].get<double>();
}

struct LowResourceConfig {
    bool enabled{false};
    uint64_t chunk_size{32 * 1024 * 1024};
    int parallel_uploads{2};
    int parallel_downloads{2};
    int hasher_threads{1};
    bool sequential_download{true};
    uint64_t max_no_compress_size{500 * 1024 * 1024};
};

inline void to_json(nlohmann::json& j, const LowResourceConfig& l) {
    j = nlohmann::json{
        {"enabled", l.enabled},
        {"chunk_size", l.chunk_size},
        {"parallel_uploads", l.parallel_uploads},
        {"parallel_downloads", l.parallel_downloads},
        {"hasher_threads", l.hasher_threads},
        {"sequential_download", l.sequential_download},
        {"max_no_compress_size", l.max_no_compress_size}
    };
}

inline void from_json(const nlohmann::json& j, LowResourceConfig& l) {
    if (j.contains("enabled") && !j["enabled"].is_null()) {
        l.enabled = j["enabled"].get<bool>();
    } else if (j.contains("low_resource_mode") && !j["low_resource_mode"].is_null()) {
        l.enabled = j["low_resource_mode"].get<bool>();
    }

    if (j.contains("chunk_size") && !j["chunk_size"].is_null()) {
        l.chunk_size = j["chunk_size"].get<uint64_t>();
    } else if (j.contains("low_resource_chunk_size") && !j["low_resource_chunk_size"].is_null()) {
        l.chunk_size = j["low_resource_chunk_size"].get<uint64_t>();
    }

    if (j.contains("parallel_uploads") && !j["parallel_uploads"].is_null()) {
        l.parallel_uploads = j["parallel_uploads"].get<int>();
    } else if (j.contains("low_resource_parallelism") && !j["low_resource_parallelism"].is_null()) {
        l.parallel_uploads = j["low_resource_parallelism"].get<int>();
    }

    if (j.contains("parallel_downloads") && !j["parallel_downloads"].is_null()) {
        l.parallel_downloads = j["parallel_downloads"].get<int>();
    } else if (j.contains("low_resource_parallelism") && !j["low_resource_parallelism"].is_null()) {
        l.parallel_downloads = j["low_resource_parallelism"].get<int>();
    }

    if (j.contains("hasher_threads") && !j["hasher_threads"].is_null()) {
        l.hasher_threads = j["hasher_threads"].get<int>();
    } else if (j.contains("low_resource_hash_workers") && !j["low_resource_hash_workers"].is_null()) {
        l.hasher_threads = j["low_resource_hash_workers"].get<int>();
    }

    if (j.contains("sequential_download") && !j["sequential_download"].is_null()) {
        l.sequential_download = j["sequential_download"].get<bool>();
    }
    if (j.contains("max_no_compress_size") && !j["max_no_compress_size"].is_null()) {
        l.max_no_compress_size = j["max_no_compress_size"].get<uint64_t>();
    }
}

struct TelegramConfig {
    int32_t api_id{0};
    std::string api_hash;
    std::string phone;
};

inline void to_json(nlohmann::json& j, const TelegramConfig& t) {
    j = nlohmann::json{
        {"api_id", t.api_id},
        {"api_hash", t.api_hash},
        {"phone", t.phone}
    };
}

inline void from_json(const nlohmann::json& j, TelegramConfig& t) {
    if (j.contains("api_id") && !j["api_id"].is_null()) {
        t.api_id = j["api_id"].get<int32_t>();
    }
    if (j.contains("api_hash") && !j["api_hash"].is_null()) {
        t.api_hash = j["api_hash"].get<std::string>();
    }
    if (j.contains("phone") && !j["phone"].is_null()) {
        t.phone = j["phone"].get<std::string>();
    }
}

struct Config {
    int64_t channel_id{0};
    int64_t index_msg_id{0};
    int64_t snapshot_index_msg_id{0};
    uint64_t chunk_size{256 * 1024 * 1024};
    bool compression{true};
    bool encryption{true};
    int parallel_uploads{8};
    int parallel_downloads{10};
    bool use_async_io{true};
    RetryConfig retry;
    LowResourceConfig low_resource;
    TelegramConfig telegram;
    std::string config_dir;
    std::string data_dir;
};

inline void to_json(nlohmann::json& j, const Config& c) {
    j = nlohmann::json{
        {"channel_id", c.channel_id},
        {"index_msg_id", c.index_msg_id},
        {"snapshot_index_msg_id", c.snapshot_index_msg_id},
        {"chunk_size", c.chunk_size},
        {"compression", c.compression},
        {"encryption", c.encryption},
        {"parallel_uploads", c.parallel_uploads},
        {"parallel_downloads", c.parallel_downloads},
        {"use_async_io", c.use_async_io},
        {"retry", c.retry},
        {"low_resource", c.low_resource},
        {"telegram", c.telegram},
        // Flat aliases for Python Config compatibility
        {"max_retries", c.retry.max_retries},
        {"retry_delay", c.retry.retry_delay},
        {"low_resource_mode", c.low_resource.enabled},
        {"low_resource_chunk_size", c.low_resource.chunk_size},
        {"low_resource_parallelism", c.low_resource.parallel_uploads},
        {"low_resource_hash_workers", c.low_resource.hasher_threads}
    };
}

inline void from_json(const nlohmann::json& j, Config& c) {
    if (j.contains("channel_id") && !j["channel_id"].is_null()) {
        c.channel_id = j["channel_id"].get<int64_t>();
    }
    if (j.contains("index_msg_id") && !j["index_msg_id"].is_null()) {
        c.index_msg_id = j["index_msg_id"].get<int64_t>();
    }
    if (j.contains("snapshot_index_msg_id") && !j["snapshot_index_msg_id"].is_null()) {
        c.snapshot_index_msg_id = j["snapshot_index_msg_id"].get<int64_t>();
    }
    if (j.contains("chunk_size") && !j["chunk_size"].is_null()) {
        c.chunk_size = j["chunk_size"].get<uint64_t>();
    }
    if (j.contains("compression") && !j["compression"].is_null()) {
        c.compression = j["compression"].get<bool>();
    }
    if (j.contains("encryption") && !j["encryption"].is_null()) {
        c.encryption = j["encryption"].get<bool>();
    }
    if (j.contains("parallel_uploads") && !j["parallel_uploads"].is_null()) {
        c.parallel_uploads = j["parallel_uploads"].get<int>();
    }
    if (j.contains("parallel_downloads") && !j["parallel_downloads"].is_null()) {
        c.parallel_downloads = j["parallel_downloads"].get<int>();
    }
    if (j.contains("use_async_io") && !j["use_async_io"].is_null()) {
        c.use_async_io = j["use_async_io"].get<bool>();
    }
    if (j.contains("config_dir") && !j["config_dir"].is_null()) {
        c.config_dir = j["config_dir"].get<std::string>();
    }
    if (j.contains("data_dir") && !j["data_dir"].is_null()) {
        c.data_dir = j["data_dir"].get<std::string>();
    }

    // Parse nested or flat retry
    if (j.contains("retry") && j["retry"].is_object()) {
        c.retry = j["retry"].get<RetryConfig>();
    } else {
        if (j.contains("max_retries") && !j["max_retries"].is_null()) {
            c.retry.max_retries = j["max_retries"].get<int>();
        }
        if (j.contains("retry_delay") && !j["retry_delay"].is_null()) {
            c.retry.retry_delay = j["retry_delay"].get<double>();
        }
    }

    // Parse nested or flat low_resource
    if (j.contains("low_resource") && j["low_resource"].is_object()) {
        c.low_resource = j["low_resource"].get<LowResourceConfig>();
    } else {
        if (j.contains("low_resource_mode") && !j["low_resource_mode"].is_null()) {
            c.low_resource.enabled = j["low_resource_mode"].get<bool>();
        }
        if (j.contains("low_resource_chunk_size") && !j["low_resource_chunk_size"].is_null()) {
            c.low_resource.chunk_size = j["low_resource_chunk_size"].get<uint64_t>();
        }
        if (j.contains("low_resource_parallelism") && !j["low_resource_parallelism"].is_null()) {
            c.low_resource.parallel_uploads = j["low_resource_parallelism"].get<int>();
            c.low_resource.parallel_downloads = c.low_resource.parallel_uploads;
        }
        if (j.contains("low_resource_hash_workers") && !j["low_resource_hash_workers"].is_null()) {
            c.low_resource.hasher_threads = j["low_resource_hash_workers"].get<int>();
        }
    }

    // Parse telegram
    if (j.contains("telegram") && j["telegram"].is_object()) {
        c.telegram = j["telegram"].get<TelegramConfig>();
    }
}

} // namespace tv
