#pragma once

#include <functional>
#include <chrono>
#include <random>
#include <thread>
#include "../models/config.hpp"

namespace tv {

template<typename T>
struct RetryResult {
    T value;
    int attempts{};
    std::chrono::milliseconds total_duration{};
};

class Retry {
public:
    explicit Retry(RetryConfig cfg = {}) : cfg_(cfg) {}

    template<typename Func>
    RetryResult<std::invoke_result_t<Func>> execute(Func&& fn) {
        using ResultType = std::invoke_result_t<Func>;
        RetryResult<ResultType> result;
        auto start = std::chrono::steady_clock::now();

        for (int attempt = 0; attempt <= cfg_.max_retries; ++attempt) {
            try {
                if constexpr (std::is_void_v<ResultType>) {
                    fn();
                } else {
                    result.value = fn();
                }
                result.attempts = attempt + 1;
                result.total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);
                return result;
            } catch (const std::exception& e) {
                if (attempt == cfg_.max_retries) throw;
                auto delay = compute_delay(attempt);
                std::this_thread::sleep_for(delay);
            }
        }
        throw std::runtime_error("Retry exhausted");
    }

    [[nodiscard]] std::chrono::milliseconds compute_delay(int attempt) const;

private:
    RetryConfig cfg_;
    mutable std::mt19937 rng_{std::random_device{}()};
};

} // namespace tv
