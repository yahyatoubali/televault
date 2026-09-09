#include "progress.hpp"
#include "../util/format.hpp"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <unistd.h>

namespace tv {

void SpeedTracker::add_sample(uint64_t bytes) {
    if (total_bytes_ == 0) {
        start_ = std::chrono::steady_clock::now();
    }
    total_bytes_ += bytes;
}

void SpeedTracker::reset() {
    total_bytes_ = 0;
    start_ = std::chrono::steady_clock::now();
}

double SpeedTracker::speed() const {
    if (total_bytes_ == 0) {
        return 0.0;
    }
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(now - start_).count();
    if (elapsed <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(total_bytes_) / elapsed;
}

std::string SpeedTracker::formatted_speed() const {
    double spd = speed();
    return format_speed(spd);
}

void ProgressBar::update(uint64_t current, uint64_t total, const std::string& suffix) {
    bool is_interactive = ::isatty(fileno(stdout));
    if (!is_interactive) {
        return; // Suppress continuous bar updates on non-interactive pipes
    }

    int width = 30;
    double progress = (total > 0) ? std::min(1.0, static_cast<double>(current) / static_cast<double>(total)) : 0.0;
    int pos = static_cast<int>(width * progress);

    std::cout << "\r[";
    for (int i = 0; i < width; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] " << static_cast<int>(progress * 100.0) << "% "
              << format_size(current) << "/" << format_size(total);
    if (!suffix.empty()) {
        std::cout << " " << suffix;
    }
    std::cout << std::flush;
}

void ProgressBar::finish() {
    if (::isatty(fileno(stdout))) {
        std::cout << std::endl;
    }
}

void ProgressBar::set_message(const std::string& msg) {
    if (::isatty(fileno(stdout))) {
        std::cout << "\r" << msg << std::flush;
    }
}

} // namespace tv
