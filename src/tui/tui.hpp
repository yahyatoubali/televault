#pragma once

#include "../core/vault.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tv {


namespace tui {
enum class FileFilterCategory {
    All,
    Media,
    Documents,
    Code,
    Archives,
    Other
};

std::string get_file_icon(const std::string& filename);
std::string format_file_size(uint64_t bytes);
std::string format_time_point(std::chrono::system_clock::time_point tp);
FileFilterCategory classify_category(const std::string& filename);
bool is_media_file(const std::string& filename);
std::string get_default_opener_command(const std::string& path_or_url);
std::vector<int> filter_file_indices(const std::vector<FileEntry>& files, const std::string& query);
std::vector<int> filter_file_indices_categorized(
    const std::vector<FileEntry>& files,
    const std::string& query,
    FileFilterCategory category = FileFilterCategory::All);
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

