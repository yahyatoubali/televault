#include "retry.hpp"
#include <algorithm>
#include <cmath>

namespace tv {

std::chrono::milliseconds Retry::compute_delay(int attempt) const {
    int64_t delay = cfg_.base_delay_ms * static_cast<int64_t>(std::pow(2, attempt));
    delay = std::min(delay, static_cast<int64_t>(cfg_.max_delay_ms));

    std::uniform_real_distribution<double> dist(
        -cfg_.jitter_factor * delay,
        cfg_.jitter_factor * delay
    );
    delay += static_cast<int64_t>(dist(rng_));
    return std::chrono::milliseconds(std::max<int64_t>(delay, 1));
}

} // namespace tv
