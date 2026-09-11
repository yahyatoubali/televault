#include "tui.hpp"
#include "../core/vault.hpp"
#include "../util/format.hpp"
#include "../webdav/stream_server.hpp"
#include "../webdav/share_server.hpp"
#include "../preview/preview.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace tv {

namespace tui {

std::string get_file_icon(const std::string& filename) {
    auto dot_pos = filename.find_last_of('.');
    std::string ext = (dot_pos != std::string::npos) ? filename.substr(dot_pos + 1) : "";
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "webp" ||
        ext == "svg" || ext == "bmp" || ext == "ico" || ext == "tiff") {
        return "🖼️ ";
    }
    if (ext == "mp4" || ext == "mkv" || ext == "avi" || ext == "mov" || ext == "webm" ||
        ext == "m4v" || ext == "wmv" || ext == "flv") {
        return "🎬 ";
    }
    if (ext == "mp3" || ext == "wav" || ext == "ogg" || ext == "flac" || ext == "aac" ||
        ext == "m4a" || ext == "opus") {
        return "🎵 ";
    }
    if (ext == "zip" || ext == "tar" || ext == "gz" || ext == "bz2" || ext == "xz" ||
        ext == "7z" || ext == "rar" || ext == "zst") {
        return "📦 ";
    }
    if (ext == "cpp" || ext == "hpp" || ext == "c" || ext == "h" || ext == "py" ||
        ext == "js" || ext == "ts" || ext == "rs" || ext == "go" || ext == "sh" ||
        ext == "json" || ext == "yaml" || ext == "toml" || ext == "html" || ext == "css") {
        return "💻 ";
    }
    if (ext == "pdf" || ext == "doc" || ext == "docx" || ext == "txt" || ext == "md" ||
        ext == "csv" || ext == "xlsx" || ext == "pptx") {
        return "📄 ";
    }
    return "📁 ";
}

std::string format_file_size(uint64_t bytes) {
    constexpr uint64_t KB = 1024;
    constexpr uint64_t MB = KB * 1024;
    constexpr uint64_t GB = MB * 1024;
    constexpr uint64_t TB = GB * 1024;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);

    if (bytes >= TB) {
        ss << (static_cast<double>(bytes) / static_cast<double>(TB)) << " TB";
    } else if (bytes >= GB) {
        ss << (static_cast<double>(bytes) / static_cast<double>(GB)) << " GB";
    } else if (bytes >= MB) {
        ss << (static_cast<double>(bytes) / static_cast<double>(MB)) << " MB";
    } else if (bytes >= KB) {
        ss << (static_cast<double>(bytes) / static_cast<double>(KB)) << " KB";
    } else {
        ss << bytes << " B";
    }
    return ss.str();
}

std::string format_time_point(std::chrono::system_clock::time_point tp) {
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &time_t_val);
#else
    localtime_r(&time_t_val, &tm_buf);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M");
    return ss.str();
}

FileFilterCategory classify_category(const std::string& filename) {
    auto dot_pos = filename.find_last_of('.');
    if (dot_pos == std::string::npos) return FileFilterCategory::Other;
    std::string ext = filename.substr(dot_pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (ext == "mp4" || ext == "mkv" || ext == "avi" || ext == "mov" || ext == "webm" ||
        ext == "m4v" || ext == "wmv" || ext == "flv" || ext == "mp3" || ext == "wav" ||
        ext == "ogg" || ext == "flac" || ext == "aac" || ext == "m4a" || ext == "opus" ||
        ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "webp" ||
        ext == "svg" || ext == "bmp") {
        return FileFilterCategory::Media;
    }
    if (ext == "pdf" || ext == "doc" || ext == "docx" || ext == "txt" || ext == "md" ||
        ext == "csv" || ext == "xlsx" || ext == "pptx" || ext == "epub") {
        return FileFilterCategory::Documents;
    }
    if (ext == "cpp" || ext == "hpp" || ext == "c" || ext == "h" || ext == "py" ||
        ext == "js" || ext == "ts" || ext == "rs" || ext == "go" || ext == "sh" ||
        ext == "json" || ext == "yaml" || ext == "toml" || ext == "html" || ext == "css" ||
        ext == "sql" || ext == "cmake") {
        return FileFilterCategory::Code;
    }
    if (ext == "zip" || ext == "tar" || ext == "gz" || ext == "bz2" || ext == "xz" ||
        ext == "7z" || ext == "rar" || ext == "zst" || ext == "iso") {
        return FileFilterCategory::Archives;
    }
    return FileFilterCategory::Other;
}

bool is_media_file(const std::string& filename) {
    return classify_category(filename) == FileFilterCategory::Media;
}

std::string get_default_opener_command(const std::string& path_or_url) {
#if defined(_WIN32)
    return "cmd.exe /c start \"\" \"" + path_or_url + "\"";
#elif defined(__APPLE__)
    return "open \"" + path_or_url + "\"";
#else
    return "xdg-open \"" + path_or_url + "\"";
#endif
}

std::vector<int> filter_file_indices(const std::vector<FileEntry>& files, const std::string& query) {
    return filter_file_indices_categorized(files, query, FileFilterCategory::All);
}

std::vector<int> filter_file_indices_categorized(
    const std::vector<FileEntry>& files,
    const std::string& query,
    FileFilterCategory category)
{
    std::vector<int> result;
    std::string query_lower = query;
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    for (size_t i = 0; i < files.size(); ++i) {
        const auto& file = files[i];

        if (category != FileFilterCategory::All) {
            if (classify_category(file.name) != category) {
                continue;
            }
        }

        if (query_lower.empty()) {
            result.push_back(static_cast<int>(i));
            continue;
        }

        std::string name_lower = file.name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (name_lower.find(query_lower) != std::string::npos ||
            file.id.find(query_lower) != std::string::npos) {
            result.push_back(static_cast<int>(i));
        }
    }
    return result;
}

void launch_external_async(const std::string& command) {
    std::thread([command]() {
#if defined(_WIN32)
        (void)::system(command.c_str());
#else
        std::string bg_cmd = "(" + command + ") >/dev/null 2>&1 &";
        (void)::system(bg_cmd.c_str());
#endif
    }).detach();
}

} // namespace tui

using namespace tui;

namespace {

enum ModalMode : int {
    ModalNone = 0,
    ModalUpload = 1,
    ModalDownload = 2,
    ModalPreview = 3,
    ModalDeleteConfirm = 4,
    ModalShare = 5,
    ModalHelp = 6
};

enum ViewTab : int {
    ViewTabFiles = 0,
    ViewTabTrash = 1,
    ViewTabStats = 2
};

enum class ActiveFocusArea {
    Sidebar,
    Search,
    FileTable,
    Inspector
};

// ── Interactive File Table Component ─────────────────────────────────────
class FileTableComponent : public ftxui::ComponentBase {
public:
    using SelectCallback = std::function<void(int)>;
    using ActionCallback = std::function<void()>;
    using FocusChangeCallback = std::function<void(ActiveFocusArea)>;

    FileTableComponent(
        const std::vector<FileEntry>& files,
        const std::vector<int>& filtered_indices,
        int& selected_index,
        SelectCallback on_select,
        ActionCallback on_open,
        FocusChangeCallback on_focus_change)
        : files_(files),
          filtered_indices_(filtered_indices),
          selected_index_(selected_index),
          on_select_(std::move(on_select)),
          on_open_(std::move(on_open)),
          on_focus_change_(std::move(on_focus_change)) {}

    bool Focusable() const override { return true; }

    bool OnEvent(ftxui::Event event) override {
        if (filtered_indices_.empty()) return false;

        // Mouse interaction
        if (event.is_mouse()) {
            const auto& mouse = event.mouse();
            if (box_.Contain(mouse.x, mouse.y)) {
                TakeFocus();
                if (on_focus_change_) on_focus_change_(ActiveFocusArea::FileTable);

                if (mouse.button == ftxui::Mouse::WheelUp) {
                    move_selection(-1);
                    return true;
                }
                if (mouse.button == ftxui::Mouse::WheelDown) {
                    move_selection(1);
                    return true;
                }

                if (mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Pressed) {
                    // Header row = y_min + 1, separator = y_min + 2, rows start at y_min + 3
                    int clicked_visual_row = mouse.y - (box_.y_min + 3);
                    if (clicked_visual_row >= 0 && clicked_visual_row < visible_count_) {
                        int target_index = scroll_offset_ + clicked_visual_row;
                        if (target_index >= 0 && target_index < static_cast<int>(filtered_indices_.size())) {
                            if (target_index == selected_index_) {
                                if (on_open_) on_open_();
                            } else {
                                selected_index_ = target_index;
                                if (on_select_) on_select_(selected_index_);
                            }
                            return true;
                        }
                    }
                }
            }
        }

        // Keyboard Navigation
        if (event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k')) {
            move_selection(-1);
            return true;
        }
        if (event == ftxui::Event::ArrowDown || event == ftxui::Event::Character('j')) {
            move_selection(1);
            return true;
        }
        if (event == ftxui::Event::PageUp) {
            move_selection(-10);
            return true;
        }
        if (event == ftxui::Event::PageDown) {
            move_selection(10);
            return true;
        }
        if (event == ftxui::Event::Home || event == ftxui::Event::Character('g')) {
            selected_index_ = 0;
            clamp_and_notify();
            return true;
        }
        if (event == ftxui::Event::End || event == ftxui::Event::Character('G')) {
            selected_index_ = static_cast<int>(filtered_indices_.size()) - 1;
            clamp_and_notify();
            return true;
        }
        if (event == ftxui::Event::Return || event == ftxui::Event::Character('o') || event == ftxui::Event::Character('O')) {
            if (on_open_) on_open_();
            return true;
        }
        if (event == ftxui::Event::ArrowLeft || event == ftxui::Event::Character('h')) {
            if (on_focus_change_) on_focus_change_(ActiveFocusArea::Sidebar);
            return true;
        }
        if (event == ftxui::Event::ArrowRight || event == ftxui::Event::Character('l')) {
            if (on_focus_change_) on_focus_change_(ActiveFocusArea::Inspector);
            return true;
        }

        return false;
    }

    ftxui::Element Render() override {
        using namespace ftxui;

        Elements rows;

        // Table Header
        rows.push_back(hbox({
            text("   ") | bold,
            text("Name") | bold | color(Color::Cyan) | flex,
            text("Size      ") | bold | color(Color::Cyan),
            text("Chunks  ") | bold | color(Color::Cyan),
            text("Security      ") | bold | color(Color::Cyan),
            text("Version ") | bold | color(Color::Cyan),
            text("Modified           ") | bold | color(Color::Cyan),
        }) | bgcolor(Color::RGB(24, 30, 42)));
        rows.push_back(separator());

        if (filtered_indices_.empty()) {
            rows.push_back(
                hbox({
                    filler(),
                    text("  (No files match current filter or channel is empty)  ") | color(Color::GrayLight) | dim,
                    filler()
                }) | size(HEIGHT, EQUAL, 6)
            );
        } else {
            clamp_scroll();
            int max_display = std::min<int>(filtered_indices_.size(), scroll_offset_ + 25);
            visible_count_ = max_display - scroll_offset_;

            for (int i = scroll_offset_; i < max_display; ++i) {
                int file_idx = filtered_indices_[i];
                if (file_idx < 0 || file_idx >= static_cast<int>(files_.size())) continue;

                const auto& file = files_[file_idx];
                bool is_selected = (i == selected_index_);

                std::string icon = get_file_icon(file.name);
                std::string enc_tag = file.encrypted ? "✓ AES-256" : "○ Plain  ";
                auto enc_color = file.encrypted ? Color::Green : Color::GrayLight;

                std::string ver_str = std::format("v{}", file.version > 0 ? file.version : 1);

                auto row_content = hbox({
                    text(is_selected ? " ▶ " : "   ") | color(Color::Cyan) | bold,
                    text(icon + file.name) | flex,
                    text(std::format("{:>9}", format_file_size(file.size))),
                    text("  "),
                    text(std::format("{:>6}", file.chunk_count)),
                    text("  "),
                    text(enc_tag) | color(enc_color),
                    text("  "),
                    text(std::format("{:<7}", ver_str)) | color(Color::Yellow),
                    text(" "),
                    text(format_time_point(file.created_at)),
                    text(" "),
                });

                if (is_selected) {
                    if (Focused()) {
                        row_content = row_content | inverted | bold;
                    } else {
                        row_content = row_content | bgcolor(Color::RGB(35, 45, 65)) | bold;
                    }
                } else if (i % 2 == 1) {
                    row_content = row_content | bgcolor(Color::RGB(18, 22, 30));
                }

                rows.push_back(row_content);
            }
        }

        auto border_style = Focused() ? (borderRounded | color(Color::Cyan)) : borderRounded;
        return vbox(std::move(rows)) | flex | border_style | reflect(box_);
    }

private:
    void move_selection(int delta) {
        if (filtered_indices_.empty()) return;
        selected_index_ = std::clamp<int>(
            selected_index_ + delta, 0, static_cast<int>(filtered_indices_.size()) - 1);
        clamp_and_notify();
    }

    void clamp_and_notify() {
        if (filtered_indices_.empty()) {
            selected_index_ = 0;
        } else {
            selected_index_ = std::clamp<int>(
                selected_index_, 0, static_cast<int>(filtered_indices_.size()) - 1);
        }
        clamp_scroll();
        if (on_select_) on_select_(selected_index_);
    }

    void clamp_scroll() {
        if (selected_index_ < scroll_offset_) {
            scroll_offset_ = selected_index_;
        }
        constexpr int max_visible = 20;
        if (selected_index_ >= scroll_offset_ + max_visible) {
            scroll_offset_ = selected_index_ - max_visible + 1;
        }
        if (scroll_offset_ < 0) scroll_offset_ = 0;
    }

    const std::vector<FileEntry>& files_;
    const std::vector<int>& filtered_indices_;
    int& selected_index_;
    int scroll_offset_{0};
    int visible_count_{0};
    ftxui::Box box_;
    SelectCallback on_select_;
    ActionCallback on_open_;
    FocusChangeCallback on_focus_change_;
};

} // namespace

class TUI::Impl {
public:
    explicit Impl(TeleVault& vault) : vault_(vault) {
        refresh_files();
    }

    ~Impl() {
        if (stream_server_) {
            stream_server_->stop();
        }
        if (share_server_) {
            share_server_->stop();
        }
    }

    void run() {
        using namespace ftxui;

        auto screen = ScreenInteractive::Fullscreen();
        screen_ptr_ = &screen;

        // ── Navigation Tabs ────────────────────────────────────────────────
        const std::vector<std::string> tab_names = {
            "📁 All Files",
            "🗑️  Trash Bin",
            "📊 Storage Overview"
        };
        int active_tab_index = 0;
        auto tab_selector = Toggle(&tab_names, &active_tab_index);

        // ── Category Filters ───────────────────────────────────────────────
        const std::vector<std::string> category_names = {
            "All", "Media", "Docs", "Code", "Archives"
        };
        int active_category_index = 0;
        auto category_selector = Radiobox(&category_names, &active_category_index);

        // ── Search Input Component ─────────────────────────────────────────
        auto input_search_opt = InputOption();
        input_search_opt.on_change = [&] {
            apply_filter();
        };
        input_search_opt.on_enter = [&] {
            focus_area_ = ActiveFocusArea::FileTable;
            if (file_table_component_) {
                file_table_component_->TakeFocus();
            }
        };
        auto search_component = Input(&search_query_, "Search files... (Press / to search, Enter to jump to files)", input_search_opt);

        // ── Modal State Variables ──────────────────────────────────────────
        std::string upload_path;
        std::string upload_password;
        bool upload_encrypt = true;
        bool upload_compress = true;
        float upload_progress = 0.0f;
        std::string upload_status_text;
        bool upload_running = false;

        std::string download_dest;
        std::string download_password;
        float download_progress = 0.0f;
        std::string download_status_text;
        bool download_running = false;

        std::string preview_content;
        std::string preview_title;
        bool preview_loading = false;

        std::string delete_target_name;
        bool delete_permanent = false;

        std::string share_link_url;
        std::string share_target_name;

        // ── Dialog Inputs ──────────────────────────────────────────────────
        auto upload_input_path = Input(&upload_path, "/path/to/local/file");
        auto upload_input_pwd = Input(&upload_password, "Optional password");
        auto upload_cb_encrypt = Checkbox("Enable AES-256-GCM Encryption", &upload_encrypt);
        auto upload_cb_compress = Checkbox("Enable Zstandard Compression", &upload_compress);

        auto btn_upload_exec = Button("Start Upload", [&] {
            if (upload_running || upload_path.empty()) return;
            if (!std::filesystem::exists(upload_path)) {
                upload_status_text = "Error: Local file does not exist!";
                return;
            }
            upload_running = true;
            upload_progress = 0.0f;
            upload_status_text = "Uploading...";

            VaultOptions opts;
            opts.encrypted = upload_encrypt;
            opts.compressed = upload_compress;
            opts.password = upload_password;

            std::string path_to_push = upload_path;
            std::thread([this, path_to_push, opts, &upload_progress, &upload_status_text, &upload_running, &screen] {
                bool success = vault_.push(path_to_push, opts, [&](const ProgressInfo& info) {
                    if (info.total > 0) {
                        upload_progress = static_cast<float>(info.current) / static_cast<float>(info.total);
                    }
                    upload_status_text = std::format("{} ({}/{} - {:.1f} MB/s)",
                        info.stage, format_file_size(info.current), format_file_size(info.total),
                        info.speed / (1024.0 * 1024.0));
                    screen.Post(Event::Custom);
                });

                screen.Post([this, success, path_to_push, &upload_running, &upload_status_text] {
                    upload_running = false;
                    if (success) {
                        status_message_ = std::format("✓ Upload complete: {}",
                            std::filesystem::path(path_to_push).filename().string());
                        refresh_files();
                        modal_mode_ = ModalNone;
                    } else {
                        upload_status_text = "Upload failed. Check connection or credentials.";
                    }
                });
            }).detach();
        });

        auto btn_upload_cancel = Button("Cancel", [&] {
            if (!upload_running) modal_mode_ = ModalNone;
        });

        auto upload_container = Container::Vertical({
            upload_input_path,
            upload_input_pwd,
            upload_cb_encrypt,
            upload_cb_compress,
            Container::Horizontal({btn_upload_exec, btn_upload_cancel})
        });

        auto download_input_dest = Input(&download_dest, "/path/to/downloaded/file");
        auto download_input_pwd = Input(&download_password, "Optional password");

        auto btn_download_exec = Button("Start Download", [&] {
            if (download_running) return;
            auto* selected = get_selected_file();
            if (!selected) return;

            download_running = true;
            download_progress = 0.0f;
            download_status_text = "Starting download...";

            VaultOptions opts;
            opts.password = download_password;

            std::string vault_name = selected->name;
            std::string out_dest = download_dest.empty() ? vault_name : download_dest;

            std::thread([this, vault_name, out_dest, opts, &download_progress, &download_status_text, &download_running, &screen] {
                bool success = vault_.pull(vault_name, out_dest, opts, [&](const ProgressInfo& info) {
                    if (info.total > 0) {
                        download_progress = static_cast<float>(info.current) / static_cast<float>(info.total);
                    }
                    download_status_text = std::format("{} ({}/{} - {:.1f} MB/s)",
                        info.stage, format_file_size(info.current), format_file_size(info.total),
                        info.speed / (1024.0 * 1024.0));
                    screen.Post(Event::Custom);
                });

                screen.Post([this, success, vault_name, out_dest, &download_running, &download_status_text] {
                    download_running = false;
                    if (success) {
                        status_message_ = std::format("✓ Downloaded '{}' to '{}'", vault_name, out_dest);
                        modal_mode_ = ModalNone;
                    } else {
                        download_status_text = "Download failed. Check password or connection.";
                    }
                });
            }).detach();
        });

        auto btn_download_cancel = Button("Cancel", [&] {
            if (!download_running) modal_mode_ = ModalNone;
        });

        auto download_container = Container::Vertical({
            download_input_dest,
            download_input_pwd,
            Container::Horizontal({btn_download_exec, btn_download_cancel})
        });

        auto btn_delete_confirm = Button("Confirm", [&] {
            auto* selected = get_selected_file();
            if (selected) {
                std::string target = selected->name;
                bool ok = vault_.delete_file(target, delete_permanent);
                if (ok) {
                    status_message_ = delete_permanent
                        ? std::format("✓ Purged '{}' permanently", target)
                        : std::format("✓ Moved '{}' to trash", target);
                    refresh_files();
                } else {
                    status_message_ = std::format("✗ Failed to delete '{}'", target);
                }
            }
            modal_mode_ = ModalNone;
        });

        auto btn_delete_cancel = Button("Cancel", [&] {
            modal_mode_ = ModalNone;
        });

        auto delete_container = Container::Horizontal({
            btn_delete_confirm,
            btn_delete_cancel
        });

        auto btn_preview_close = Button("Close Preview (Esc)", [&] {
            modal_mode_ = ModalNone;
        });

        auto btn_share_close = Button("Close (Esc)", [&] {
            modal_mode_ = ModalNone;
        });

        auto btn_help_close = Button("Close (Esc)", [&] {
            modal_mode_ = ModalNone;
        });

        // ── Action Handlers ────────────────────────────────────────────────
        auto handle_open_default = [this, &screen] {
            auto* selected = get_selected_file();
            if (!selected) {
                status_message_ = "Select a file first";
                return;
            }

            std::string fname = selected->name;
            if (is_media_file(fname)) {
                ensure_stream_server();
                std::string url = stream_server_->stream_url(fname);
                status_message_ = "▶ Streaming media: " + fname;
                launch_external_async(std::format("(mpv '{}' || vlc '{}' || ffplay '{}' || {})",
                    url, url, url, get_default_opener_command(url)));
                return;
            }

            // For non-media documents/images/archives: pull to temp cache and open in OS default viewer
            status_message_ = "⏳ Opening '" + fname + "' with default application...";
            std::thread([this, fname, &screen] {
                auto temp_dir = std::filesystem::temp_directory_path() / "televault_open";
                std::filesystem::create_directories(temp_dir);
                auto local_temp = temp_dir / fname;

                VaultOptions opts;
                bool ok = true;
                if (!std::filesystem::exists(local_temp)) {
                    ok = vault_.pull(fname, local_temp.string(), opts, nullptr);
                }

                screen.Post([this, ok, fname, local_temp] {
                    if (ok) {
                        launch_external_async(get_default_opener_command(local_temp.string()));
                        status_message_ = "✓ Opened '" + fname + "' with default app";
                    } else {
                        status_message_ = "✗ Could not retrieve '" + fname + "' for opening";
                    }
                });
            }).detach();
        };

        auto handle_stream = [this] {
            auto* selected = get_selected_file();
            if (!selected) {
                status_message_ = "Select a media file to stream";
                return;
            }
            ensure_stream_server();
            std::string url = stream_server_->stream_url(selected->name);
            status_message_ = "▶ Streaming: " + selected->name + " on " + url;
            launch_external_async(std::format("(mpv '{}' || vlc '{}' || ffplay '{}' || {})",
                url, url, url, get_default_opener_command(url)));
        };

        auto handle_preview = [this, &preview_title, &preview_content, &preview_loading, &screen] {
            auto* selected = get_selected_file();
            if (!selected) {
                status_message_ = "Select a file to preview";
                return;
            }
            preview_title = selected->name;
            preview_content = "Fetching preview chunk from Telegram...";
            preview_loading = true;
            modal_mode_ = ModalPreview;

            std::string vpath = selected->name;
            std::thread([this, vpath, &preview_content, &preview_loading, &screen] {
                VaultOptions opts;
                auto chunk0 = vault_.read_first_chunk(vpath, opts);
                screen.Post([&preview_content, &preview_loading, chunk0] {
                    preview_loading = false;
                    if (!chunk0 || chunk0->empty()) {
                        preview_content = "Preview unavailable or encrypted. Use Download to decrypt.";
                        return;
                    }
                    const auto& data = *chunk0;
                    size_t preview_len = std::min<size_t>(data.size(), 4096);
                    bool is_ascii = true;
                    for (size_t i = 0; i < preview_len; ++i) {
                        if (data[i] == 0 || (!std::isprint(data[i]) && !std::isspace(data[i]))) {
                            is_ascii = false;
                            break;
                        }
                    }
                    if (is_ascii) {
                        preview_content = std::string(reinterpret_cast<const char*>(data.data()), preview_len);
                        if (data.size() > preview_len) {
                            preview_content += "\n\n... [Truncated: showing first " + format_file_size(preview_len) +
                                               " of " + format_file_size(data.size()) + "]";
                        }
                    } else {
                        std::ostringstream hex_ss;
                        hex_ss << "Binary Data (Hex View):\n";
                        for (size_t i = 0; i < std::min<size_t>(data.size(), 512); ++i) {
                            hex_ss << std::hex << std::setw(2) << std::setfill('0')
                                   << static_cast<int>(data[i]) << " ";
                            if ((i + 1) % 16 == 0) hex_ss << "\n";
                        }
                        preview_content = hex_ss.str();
                    }
                });
            }).detach();
        };

        auto handle_share = [this, &share_link_url, &share_target_name, &screen] {
            auto* selected = get_selected_file();
            if (!selected) {
                status_message_ = "Select a file to share";
                return;
            }
            ensure_share_server();
            share_target_name = selected->name;
            share_link_url = share_server_->share_url() + "/" + selected->name + "?token=" + share_server_->token();
            modal_mode_ = ModalShare;
        };

        auto handle_verify = [this, &screen] {
            auto* selected = get_selected_file();
            if (!selected) {
                status_message_ = "Select a file to verify";
                return;
            }
            std::string name = selected->name;
            status_message_ = "⏳ Verifying Blake3 integrity of '" + name + "'...";
            std::thread([this, name, &screen] {
                bool ok = vault_.verify_file(name);
                screen.Post([this, ok, name] {
                    if (ok) {
                        status_message_ = "✓ Integrity verified (Blake3 hash valid): " + name;
                    } else {
                        status_message_ = "✗ Integrity check FAILED: " + name;
                    }
                });
            }).detach();
        };

        auto handle_sync = [this, &screen] {
            status_message_ = "⏳ Synchronizing vault index with Telegram channel...";
            std::thread([this, &screen] {
                bool ok = vault_.sync(true);
                screen.Post([this, ok] {
                    refresh_files();
                    if (ok) {
                        status_message_ = std::format("✓ Synced with Telegram: {} files active", files_.size());
                    } else {
                        status_message_ = "✗ Failed to synchronize with channel";
                    }
                });
            }).detach();
        };

        auto handle_restore = [this] {
            auto* selected = get_selected_file();
            if (!selected) {
                status_message_ = "Select a file to restore";
                return;
            }
            if (vault_.restore_file(selected->name)) {
                status_message_ = "✓ Restored '" + selected->name + "' from trash";
                refresh_files();
            } else {
                status_message_ = "✗ Failed to restore file";
            }
        };

        auto handle_empty_trash = [this] {
            if (vault_.empty_trash()) {
                status_message_ = "✓ Emptied encrypted trash";
                refresh_files();
            } else {
                status_message_ = "✗ Failed to empty trash";
            }
        };

        // ── Sidebar Action Buttons ─────────────────────────────────────────
        auto btn_open_act = Button("⚡ Open (Enter/o)", handle_open_default);
        auto btn_stream_act = Button("▶  Stream (s)", handle_stream);
        auto btn_preview_act = Button("👁  Preview (p)", handle_preview);
        auto btn_upload_act = Button("⬆  Upload (u)", [&] {
            upload_path.clear();
            upload_password.clear();
            upload_progress = 0.0f;
            upload_status_text.clear();
            upload_running = false;
            modal_mode_ = ModalUpload;
        });
        auto btn_download_act = Button("⬇  Download (d)", [&] {
            auto* selected = get_selected_file();
            if (selected) {
                download_dest = selected->name;
                download_password.clear();
                download_progress = 0.0f;
                download_status_text.clear();
                download_running = false;
                modal_mode_ = ModalDownload;
            } else {
                status_message_ = "Select a file to download";
            }
        });
        auto btn_share_act = Button("🔗 Share Link (l)", handle_share);
        auto btn_verify_act = Button("🛡️  Verify (v)", handle_verify);
        auto btn_delete_act = Button("✕  Delete (x)", [&] {
            auto* selected = get_selected_file();
            if (selected) {
                delete_target_name = selected->name;
                delete_permanent = (current_tab_ == ViewTabTrash);
                modal_mode_ = ModalDeleteConfirm;
            } else {
                status_message_ = "Select a file to delete";
            }
        });
        auto btn_sync_act = Button("↻  Sync (F5)", handle_sync);
        auto btn_help_act = Button("?  Help (?)", [&] { modal_mode_ = ModalHelp; });
        auto btn_quit_act = Button("⏻  Quit (q)", [&] { screen.Exit(); });

        auto sidebar_container = Container::Vertical({
            tab_selector,
            category_selector,
            btn_open_act,
            btn_stream_act,
            btn_preview_act,
            btn_download_act,
            btn_upload_act,
            btn_share_act,
            btn_verify_act,
            btn_delete_act,
            btn_sync_act,
            btn_help_act,
            btn_quit_act,
        });

        // ── Custom Interactive Table ───────────────────────────────────────
        auto table_comp = std::make_shared<FileTableComponent>(
            files_,
            filtered_indices_,
            selected_index_,
            [&](int /*idx*/) {
                // selection changed
            },
            handle_open_default,
            [&](ActiveFocusArea area) {
                focus_area_ = area;
                if (area == ActiveFocusArea::Sidebar) {
                    sidebar_container->TakeFocus();
                }
            }
        );
        file_table_component_ = table_comp;

        // ── Main Horizontal Split ──────────────────────────────────────────
        auto main_layout_container = Container::Horizontal({
            sidebar_container,
            Container::Vertical({
                search_component,
                table_comp
            })
        });

        // ── Modal Renderers ────────────────────────────────────────────────
        auto upload_modal_renderer = Renderer(upload_container, [&] {
            auto progress_gauge = upload_running ? gauge(upload_progress) | color(Color::Green) : text("");
            return vbox({
                text("⬆  Upload & Encrypt File to Vault") | bold | color(Color::Cyan),
                separator(),
                text("Local file path:"),
                upload_input_path->Render() | borderLight,
                text("Optional encryption password:"),
                upload_input_pwd->Render() | borderLight,
                upload_cb_encrypt->Render(),
                upload_cb_compress->Render(),
                separator(),
                upload_running ? vbox({
                    text(upload_status_text) | color(Color::Yellow),
                    progress_gauge
                }) : text(upload_status_text) | color(Color::Red),
                Container::Horizontal({btn_upload_exec, btn_upload_cancel})->Render()
            }) | borderRounded | size(WIDTH, GREATER_THAN, 60) | bgcolor(Color::RGB(20, 24, 32));
        });

        auto download_modal_renderer = Renderer(download_container, [&] {
            auto* selected = get_selected_file();
            std::string file_info_hdr = selected ? std::format("Target: {} ({})", selected->name, format_file_size(selected->size)) : "No file selected";
            auto progress_gauge = download_running ? gauge(download_progress) | color(Color::Green) : text("");
            return vbox({
                text("⬇  Download & Decrypt File") | bold | color(Color::Cyan),
                separator(),
                text(file_info_hdr) | color(Color::White),
                separator(),
                text("Destination path on local filesystem:"),
                download_input_dest->Render() | borderLight,
                text("Decryption Password:"),
                download_input_pwd->Render() | borderLight,
                separator(),
                download_running ? vbox({
                    text(download_status_text) | color(Color::Yellow),
                    progress_gauge
                }) : text(download_status_text) | color(Color::Red),
                Container::Horizontal({btn_download_exec, btn_download_cancel})->Render()
            }) | borderRounded | size(WIDTH, GREATER_THAN, 60) | bgcolor(Color::RGB(20, 24, 32));
        });

        auto delete_modal_renderer = Renderer(delete_container, [&] {
            std::string prompt = delete_permanent
                ? std::format("Permanently delete '{}' and purge all chunks from Telegram?", delete_target_name)
                : std::format("Move '{}' to encrypted trash?", delete_target_name);
            return vbox({
                text(delete_permanent ? "✕  Confirm Permanent Purge" : "🗑️  Move to Trash") | bold | color(Color::Red),
                separator(),
                text(prompt) | color(Color::White),
                separator(),
                Container::Horizontal({btn_delete_confirm, btn_delete_cancel})->Render()
            }) | borderRounded | size(WIDTH, GREATER_THAN, 55) | bgcolor(Color::RGB(32, 16, 16));
        });

        auto preview_modal_renderer = Renderer(btn_preview_close, [&] {
            bool is_md = preview_title.ends_with(".md") || preview_title.ends_with(".markdown");
            Elements preview_elements;
            if (is_md) {
                std::istringstream stream(preview_content);
                std::string line;
                int count = 0;
                while (std::getline(stream, line) && count < 30) {
                    if (line.starts_with("# ")) {
                        preview_elements.push_back(text("  " + line.substr(2)) | bold | color(Color::CyanLight));
                    } else if (line.starts_with("## ")) {
                        preview_elements.push_back(text("   " + line.substr(3)) | bold | color(Color::YellowLight));
                    } else if (line.starts_with("### ")) {
                        preview_elements.push_back(text("    " + line.substr(4)) | bold | color(Color::GreenLight));
                    } else if (line.starts_with("- ") || line.starts_with("* ")) {
                        preview_elements.push_back(text("  • " + line.substr(2)) | color(Color::White));
                    } else if (line.starts_with("> ")) {
                        preview_elements.push_back(text("  │ " + line.substr(2)) | dim | color(Color::GrayLight));
                    } else if (line.starts_with("```")) {
                        preview_elements.push_back(text(line) | color(Color::MagentaLight));
                    } else {
                        preview_elements.push_back(text(line) | color(Color::White));
                    }
                    count++;
                }
            } else {
                std::istringstream stream(preview_content);
                std::string line;
                int line_num = 1;
                while (std::getline(stream, line) && line_num <= 30) {
                    preview_elements.push_back(
                        hbox({
                            text(std::format("{:>3} │ ", line_num)) | color(Color::GrayLight),
                            text(line) | color(Color::White)
                        })
                    );
                    line_num++;
                }
            }

            return vbox({
                hbox({
                    text(std::format("👁  Preview: {}", preview_title)) | bold | color(Color::Cyan),
                    filler(),
                    text(preview_loading ? "⏳ Loading..." : "") | color(Color::Yellow)
                }),
                separator(),
                vbox(std::move(preview_elements)) | size(HEIGHT, GREATER_THAN, 12) | size(HEIGHT, LESS_THAN, 24),
                separator(),
                btn_preview_close->Render()
            }) | borderRounded | size(WIDTH, GREATER_THAN, 75) | bgcolor(Color::RGB(20, 24, 32));
        });

        auto share_modal_renderer = Renderer(btn_share_close, [&] {
            return vbox({
                text("🔗 Ephemeral Share Link Generated") | bold | color(Color::Green),
                separator(),
                text(std::format("File: {}", share_target_name)) | color(Color::White),
                text("Direct Download URL (temporary token):") | color(Color::GrayLight),
                text("  " + share_link_url) | bold | color(Color::Cyan) | borderLight,
                text("Open in browser or copy URL to download without Telegram account.") | color(Color::GrayLight),
                separator(),
                btn_share_close->Render()
            }) | borderRounded | size(WIDTH, GREATER_THAN, 70) | bgcolor(Color::RGB(20, 24, 32));
        });

        auto help_modal_renderer = Renderer(btn_help_close, [&] {
            return vbox({
                text("TeleVault TUI - Keybindings & Features") | bold | color(Color::Cyan),
                separator(),
                hbox({text("  [Enter / o] ") | bold | color(Color::Yellow), text("Open file in OS default application (or media player)")}),
                hbox({text("  [s]         ") | bold | color(Color::Yellow), text("Stream media via HTTP Range (mpv/vlc/ffplay)")}),
                hbox({text("  [p]         ") | bold | color(Color::Yellow), text("Instant text, code, or hex preview modal")}),
                hbox({text("  [d]         ") | bold | color(Color::Yellow), text("Download file from vault with decryption")}),
                hbox({text("  [u]         ") | bold | color(Color::Yellow), text("Upload & encrypt local file into vault")}),
                hbox({text("  [l]         ") | bold | color(Color::Yellow), text("Generate direct ephemeral share link")}),
                hbox({text("  [v]         ") | bold | color(Color::Yellow), text("Verify Blake3 cryptographic hash integrity")}),
                hbox({text("  [x / Del]   ") | bold | color(Color::Yellow), text("Move file to trash (or purge if in Trash view)")}),
                hbox({text("  [r]         ") | bold | color(Color::Yellow), text("Restore file (in Trash view)")}),
                hbox({text("  [F5 / R]    ") | bold | color(Color::Yellow), text("Sync index with Telegram channel")}),
                hbox({text("  [/]         ") | bold | color(Color::Yellow), text("Focus search filter bar")}),
                hbox({text("  [Tab]       ") | bold | color(Color::Yellow), text("Cycle focus between Sidebar, Search, and File Table")}),
                hbox({text("  [↑ / ↓]     ") | bold | color(Color::Yellow), text("Navigate rows (also j/k, PgUp/PgDn, Home/End)")}),
                hbox({text("  [1, 2, 3]   ") | bold | color(Color::Yellow), text("Switch tabs: [1] Files, [2] Trash, [3] Stats")}),
                hbox({text("  [Esc]       ") | bold | color(Color::Yellow), text("Close active modal or unfocus search")}),
                hbox({text("  [q]         ") | bold | color(Color::Yellow), text("Quit TeleVault TUI")}),
                separator(),
                btn_help_close->Render()
            }) | borderRounded | size(WIDTH, GREATER_THAN, 68) | bgcolor(Color::RGB(20, 24, 32));
        });

        // ── Main View Renderer ─────────────────────────────────────────────
        auto main_renderer = Renderer(main_layout_container, [&] {
            // Check if tab changed
            if (active_tab_index != static_cast<int>(current_tab_)) {
                current_tab_ = static_cast<ViewTab>(active_tab_index);
                refresh_files();
            }
            if (active_category_index != static_cast<int>(current_category_)) {
                current_category_ = static_cast<FileFilterCategory>(active_category_index);
                apply_filter();
            }

            // Header Banner
            auto header_elem = hbox({
                text(" 🛡️  TELEVAULT v4.0.0 ") | bold | color(Color::Cyan),
                text("│ Secure Telegram Cloud Vault (C++23) ") | color(Color::GrayLight),
                filler(),
                tab_selector->Render() | bold,
                filler(),
                text(" ● MTProto Connected ") | color(Color::Green) | bold,
            }) | bgcolor(Color::RGB(16, 20, 28));

            // Sidebar Element
            auto stats_widget = vbox({
                text(" Storage Summary ") | bold | color(Color::Cyan),
                separator(),
                hbox({text("Files:  ") | color(Color::GrayLight), text(std::to_string(files_.size())) | bold | color(Color::White)}),
                hbox({text("Size:   ") | color(Color::GrayLight), text(format_file_size(total_bytes_)) | bold | color(Color::Green)}),
                hbox({text("Match:  ") | color(Color::GrayLight), text(std::to_string(filtered_indices_.size())) | bold | color(Color::Yellow)}),
                hbox({text("Cipher: ") | color(Color::GrayLight), text("AES-256-GCM") | color(Color::Green)}),
            }) | borderLight;

            auto sidebar_elem = vbox({
                text(" Quick Filters ") | bold | color(Color::Cyan),
                category_selector->Render(),
                separator(),
                text(" Operations ") | bold | color(Color::Cyan),
                btn_open_act->Render(),
                btn_stream_act->Render(),
                btn_preview_act->Render(),
                btn_download_act->Render(),
                btn_upload_act->Render(),
                btn_share_act->Render(),
                btn_verify_act->Render(),
                btn_delete_act->Render(),
                separator(),
                btn_sync_act->Render(),
                btn_help_act->Render(),
                btn_quit_act->Render(),
                filler(),
                stats_widget
            }) | size(WIDTH, EQUAL, 25) | bgcolor(Color::RGB(18, 22, 30));

            // Selected Item Detail Inspector
            Element detail_box;
            auto* selected_file = get_selected_file();
            if (selected_file) {
                std::string hash_disp = selected_file->hash.empty() ? "(none)" : selected_file->hash;
                std::string cat_name = is_media_file(selected_file->name) ? "Media (Audio/Video/Image)" : "Document/Binary";
                std::string action_hints = (current_tab_ == ViewTabTrash)
                    ? "[r] Restore File  [x] Purge Permanently  [Shift+E] Empty Trash"
                    : "[Enter / o] Open in Default App  [p] Preview  [s] Stream  [d] Download  [l] Share Link  [v] Verify  [x] Trash";

                detail_box = vbox({
                    hbox({
                        text(" File Inspector: ") | bold | color(Color::Cyan),
                        text(selected_file->name) | bold | color(Color::White),
                        filler(),
                        text(cat_name) | color(Color::Yellow)
                    }),
                    separator(),
                    hbox({
                        text("ID: ") | color(Color::GrayLight),
                        text(selected_file->id) | color(Color::White),
                        text("  │  Blake3: ") | color(Color::GrayLight),
                        text(hash_disp) | color(Color::Yellow),
                        text("  │  Size: ") | color(Color::GrayLight),
                        text(format_file_size(selected_file->size)) | bold | color(Color::Green),
                        text("  │  Chunks: ") | color(Color::GrayLight),
                        text(std::to_string(selected_file->chunk_count)) | color(Color::White),
                    }),
                    hbox({
                        text("Shortcuts: ") | color(Color::Cyan),
                        text(action_hints) | color(Color::GrayLight)
                    })
                }) | borderLight | size(HEIGHT, EQUAL, 5);
            } else {
                detail_box = vbox({
                    text("No file selected. Use arrow keys or click to select.") | color(Color::GrayLight)
                }) | borderLight | size(HEIGHT, EQUAL, 3);
            }

            // Center Panel (Search + Table + Inspector)
            auto center_panel = vbox({
                hbox({
                    text(" Filter: ") | bold | color(Color::Yellow),
                    search_component->Render() | flex,
                    text(std::format(" [{}/{} items] ", filtered_indices_.size(), files_.size())) | color(Color::GrayLight),
                }) | borderLight,
                table_comp->Render(),
                detail_box
            }) | flex;

            // Footer Status Bar
            auto footer_elem = hbox({
                text(" Status: ") | bold | color(Color::Cyan),
                text(status_message_) | color(Color::White) | bold,
                filler(),
                text("[Tab] Focus  [o/Enter] Open  [/] Search  [?] Help  [q] Quit ") | color(Color::GrayLight),
            }) | bgcolor(Color::RGB(16, 20, 28));

            return vbox({
                header_elem,
                hbox({
                    sidebar_elem,
                    center_panel
                }) | flex,
                footer_elem
            });
        });

        // ── Modal Switcher ─────────────────────────────────────────────────
        auto modal_container = Container::Tab({
            main_renderer,
            upload_modal_renderer,
            download_modal_renderer,
            preview_modal_renderer,
            delete_modal_renderer,
            share_modal_renderer,
            help_modal_renderer,
        }, &modal_mode_);

        // ── Top Event Handler (Global Shortcuts & Focus Cycling) ───────────
        auto top_component = CatchEvent(modal_container, [&](Event event) {
            // Modal active handling
            if (modal_mode_ != ModalNone) {
                if (event == Event::Escape) {
                    if (!upload_running && !download_running) {
                        modal_mode_ = ModalNone;
                        return true;
                    }
                }
                return false;
            }

            // Global Keybindings
            if (event == Event::Character('q') || event == Event::Character('Q')) {
                screen.Exit();
                return true;
            }
            if (event == Event::F5 || event == Event::Character('R')) {
                handle_sync();
                return true;
            }
            if (event == Event::Character('1')) {
                active_tab_index = 0;
                current_tab_ = ViewTabFiles;
                refresh_files();
                return true;
            }
            if (event == Event::Character('2')) {
                active_tab_index = 1;
                current_tab_ = ViewTabTrash;
                refresh_files();
                return true;
            }
            if (event == Event::Character('3')) {
                active_tab_index = 2;
                current_tab_ = ViewTabStats;
                refresh_files();
                return true;
            }
            if (event == Event::Character('?') || event == Event::F1) {
                modal_mode_ = ModalHelp;
                return true;
            }
            if (event == Event::Character('/')) {
                search_component->TakeFocus();
                focus_area_ = ActiveFocusArea::Search;
                return true;
            }
            if (event == Event::Escape) {
                if (!search_query_.empty()) {
                    search_query_.clear();
                    apply_filter();
                }
                table_comp->TakeFocus();
                focus_area_ = ActiveFocusArea::FileTable;
                return true;
            }
            if (event == Event::Tab) {
                if (focus_area_ == ActiveFocusArea::Sidebar) {
                    search_component->TakeFocus();
                    focus_area_ = ActiveFocusArea::Search;
                } else if (focus_area_ == ActiveFocusArea::Search) {
                    table_comp->TakeFocus();
                    focus_area_ = ActiveFocusArea::FileTable;
                } else {
                    sidebar_container->TakeFocus();
                    focus_area_ = ActiveFocusArea::Sidebar;
                }
                return true;
            }

            // Shortcuts when not typing in search box
            if (focus_area_ != ActiveFocusArea::Search) {
                if (event == Event::Character('o') || event == Event::Character('O')) {
                    handle_open_default();
                    return true;
                }
                if (event == Event::Character('s') || event == Event::Character('S')) {
                    handle_stream();
                    return true;
                }
                if (event == Event::Character('p') || event == Event::Character('P')) {
                    handle_preview();
                    return true;
                }
                if (event == Event::Character('l')) {
                    handle_share();
                    return true;
                }
                if (event == Event::Character('v') || event == Event::Character('V')) {
                    handle_verify();
                    return true;
                }
                if (event == Event::Character('u') || event == Event::Character('U')) {
                    upload_path.clear();
                    upload_password.clear();
                    upload_progress = 0.0f;
                    upload_status_text.clear();
                    upload_running = false;
                    modal_mode_ = ModalUpload;
                    return true;
                }
                if (event == Event::Character('d') || event == Event::Character('D')) {
                    auto* selected = get_selected_file();
                    if (selected) {
                        download_dest = selected->name;
                        download_password.clear();
                        download_progress = 0.0f;
                        download_status_text.clear();
                        download_running = false;
                        modal_mode_ = ModalDownload;
                    } else {
                        status_message_ = "Select a file to download";
                    }
                    return true;
                }
                if (event == Event::Character('x') || event == Event::Character('X') || event == Event::Delete) {
                    auto* selected = get_selected_file();
                    if (selected) {
                        delete_target_name = selected->name;
                        delete_permanent = (current_tab_ == ViewTabTrash);
                        modal_mode_ = ModalDeleteConfirm;
                    }
                    return true;
                }
                if (current_tab_ == ViewTabTrash) {
                    if (event == Event::Character('r')) {
                        handle_restore();
                        return true;
                    }
                    if (event == Event::Character('E')) {
                        handle_empty_trash();
                        return true;
                    }
                }
            }

            return false;
        });

        screen.Loop(top_component);
    }

private:
    void ensure_stream_server() {
        if (!stream_server_) {
            stream_server_ = std::make_unique<StreamServer>(vault_);
            StreamOptions s_opts;
            s_opts.port = 8080;
            stream_server_->start(s_opts, false);
        }
    }

    void ensure_share_server() {
        if (!share_server_) {
            share_server_ = std::make_unique<ShareServer>(vault_);
            ShareOptions sh_opts;
            sh_opts.port = 8088;
            share_server_->start(sh_opts, false);
        }
    }

    void refresh_files() {
        if (current_tab_ == ViewTabTrash) {
            files_ = vault_.list_trash();
        } else {
            files_ = vault_.list_files();
        }

        total_bytes_ = 0;
        for (const auto& file : files_) {
            total_bytes_ += file.size;
        }
        apply_filter();
    }

    void apply_filter() {
        filtered_indices_ = filter_file_indices_categorized(
            files_, search_query_, current_category_);

        if (filtered_indices_.empty()) {
            selected_index_ = 0;
        } else if (selected_index_ >= static_cast<int>(filtered_indices_.size())) {
            selected_index_ = static_cast<int>(filtered_indices_.size()) - 1;
        }
    }

    const FileEntry* get_selected_file() const {
        if (filtered_indices_.empty() || selected_index_ < 0 ||
            selected_index_ >= static_cast<int>(filtered_indices_.size())) {
            return nullptr;
        }
        int file_idx = filtered_indices_[selected_index_];
        if (file_idx >= 0 && file_idx < static_cast<int>(files_.size())) {
            return &files_[file_idx];
        }
        return nullptr;
    }

    TeleVault& vault_;
    ftxui::ScreenInteractive* screen_ptr_{nullptr};
    std::shared_ptr<FileTableComponent> file_table_component_;
    std::vector<FileEntry> files_;
    std::vector<int> filtered_indices_;
    int selected_index_{0};
    uint64_t total_bytes_{0};
    std::string search_query_;
    std::string status_message_{"Ready - Welcome to TeleVault"};
    int modal_mode_{ModalNone};
    ViewTab current_tab_{ViewTabFiles};
    FileFilterCategory current_category_{FileFilterCategory::All};
    ActiveFocusArea focus_area_{ActiveFocusArea::FileTable};
    std::unique_ptr<StreamServer> stream_server_;
    std::unique_ptr<ShareServer> share_server_;
};

TUI::TUI(TeleVault& vault) : impl_(std::make_unique<Impl>(vault)) {}
TUI::~TUI() = default;

void TUI::run() {
    if (impl_) {
        impl_->run();
    }
}

} // namespace tv
