#pragma once

#include <string>
#include <cstdint>
#include <chrono>

namespace tv {

class SpeedTracker {
public:
    void add_sample(uint64_t bytes);
    void reset();
    [[nodiscard]] double speed() const;
    [[nodiscard]] std::string formatted_speed() const;

private:
    uint64_t total_bytes_{};
    std::chrono::steady_clock::time_point start_;
};

class ProgressBar {
public:
    void update(uint64_t current, uint64_t total, const std::string& suffix = {});
    void finish();
    void set_message(const std::string& msg);
};

} // namespace tv
