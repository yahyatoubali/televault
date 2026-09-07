#pragma once

#include "../core/vault.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tv {


namespace tui {
std::string get_file_icon(const std::string& filename);
std::string format_file_size(uint64_t bytes);
std::string format_time_point(std::chrono::system_clock::time_point tp);
std::vector<int> filter_file_indices(const std::vector<FileEntry>& files, const std::string& query);
} // namespace tui

class TUI {
public:
    explicit TUI(TeleVault& vault);
    ~TUI();

    void run();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv

