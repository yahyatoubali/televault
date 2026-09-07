#include "tui.hpp"
#include "../core/vault.hpp"
#include "../util/format.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <iomanip>
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

std::vector<int> filter_file_indices(const std::vector<FileEntry>& files, const std::string& query) {
    std::vector<int> result;
    std::string query_lower = query;
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    for (size_t i = 0; i < files.size(); ++i) {
        if (query_lower.empty()) {
            result.push_back(static_cast<int>(i));
            continue;
        }
        std::string name_lower = files[i].name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (name_lower.find(query_lower) != std::string::npos) {
            result.push_back(static_cast<int>(i));
        }
    }
    return result;
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
    ModalHelp = 5
};

} // namespace

class TUI::Impl {
public:
    explicit Impl(TeleVault& vault) : vault_(vault) {
        refresh_files();
    }

    void run() {
        using namespace ftxui;

        auto screen = ScreenInteractive::Fullscreen();
        screen_ptr_ = &screen;

        // ── Search Input Component ─────────────────────────────────────────
        auto input_search_opt = InputOption();
        input_search_opt.on_change = [&] {
            apply_filter();
        };
        auto search_component = Input(&search_query_, "Search files... (/ to focus, Esc to clear)", input_search_opt);

        // ── Modal State & Inputs ───────────────────────────────────────────
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

        // ── Modal Components ───────────────────────────────────────────────
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
                        status_message_ = std::format("✓ Successfully uploaded: {}",
                            std::filesystem::path(path_to_push).filename().string());
                        refresh_files();
                        modal_mode_ = ModalNone;
                    } else {
                        upload_status_text = "Upload failed. Check logs / credentials.";
                    }
                });
            }).detach();
        });

        auto btn_upload_cancel = Button("Cancel", [&] {
            if (!upload_running) {
                modal_mode_ = ModalNone;
            }
        });

        auto upload_container = Container::Vertical({
            upload_input_path,
            upload_input_pwd,
            upload_cb_encrypt,
            upload_cb_compress,
            Container::Horizontal({btn_upload_exec, btn_upload_cancel})
        });

        // ── Download Modal Components ──────────────────────────────────────
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
                        status_message_ = std::format("✓ Successfully downloaded '{}' to '{}'", vault_name, out_dest);
                        modal_mode_ = ModalNone;
                    } else {
                        download_status_text = "Download failed. Check password or integrity.";
                    }
                });
            }).detach();
        });

        auto btn_download_cancel = Button("Cancel", [&] {
            if (!download_running) {
                modal_mode_ = ModalNone;
            }
        });

        auto download_container = Container::Vertical({
            download_input_dest,
            download_input_pwd,
            Container::Horizontal({btn_download_exec, btn_download_cancel})
        });

        // ── Delete Confirmation Modal Components ───────────────────────────
        auto btn_delete_confirm = Button("Confirm Delete", [&] {
            auto* selected = get_selected_file();
            if (selected) {
                std::string name_to_delete = selected->name;
                bool ok = vault_.delete_file(name_to_delete);
                if (ok) {
                    status_message_ = std::format("✓ Deleted file '{}' from vault", name_to_delete);
                    refresh_files();
                } else {
                    status_message_ = std::format("✗ Failed to delete file '{}'", name_to_delete);
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

        // ── Preview Modal Components ───────────────────────────────────────
        auto btn_preview_close = Button("Close Preview", [&] {
            modal_mode_ = ModalNone;
        });

        // ── Help Modal Components ──────────────────────────────────────────
        auto btn_help_close = Button("Close", [&] {
            modal_mode_ = ModalNone;
        });

        // ── Left Sidebar Buttons ───────────────────────────────────────────
        auto btn_upload = Button("⬆  Upload (u)", [&] {
            upload_path.clear();
            upload_password.clear();
            upload_progress = 0.0f;
            upload_status_text.clear();
            upload_running = false;
            modal_mode_ = ModalUpload;
        });

        auto btn_download = Button("⬇  Download (d)", [&] {
            auto* selected = get_selected_file();
            if (selected) {
                download_dest = selected->name;
                download_password.clear();
                download_progress = 0.0f;
                download_status_text.clear();
                download_running = false;
                modal_mode_ = ModalDownload;
            } else {
                status_message_ = "Select a file to download first";
            }
        });

        auto btn_preview = Button("👁  Preview (p)", [&] {
            auto* selected = get_selected_file();
            if (selected) {
                preview_title = selected->name;
                preview_content = "Fetching chunk 0 for preview...";
                preview_loading = true;
                modal_mode_ = ModalPreview;

                std::string vpath = selected->name;
                std::thread([this, vpath, &preview_content, &preview_loading, &screen] {
                    VaultOptions opts;
                    auto chunk0 = vault_.read_first_chunk(vpath, opts);
                    screen.Post([&preview_content, &preview_loading, chunk0] {
                        preview_loading = false;
                        if (!chunk0 || chunk0->empty()) {
                            preview_content = "Preview unavailable or encrypted. Password required.";
                            return;
                        }
                        const auto& data = *chunk0;
                        size_t preview_len = std::min<size_t>(data.size(), 2048);
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
                                preview_content += "\n\n... [Preview truncated: " + format_file_size(data.size()) + " total in chunk 0]";
                            }
                        } else {
                            std::ostringstream hex_stream;
                            hex_stream << "Binary file (Magic header):\n";
                            for (size_t i = 0; i < std::min<size_t>(data.size(), 256); ++i) {
                                hex_stream << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]) << " ";
                                if ((i + 1) % 16 == 0) hex_stream << "\n";
                            }
                            preview_content = hex_stream.str();
                        }
                    });
                }).detach();
            } else {
                status_message_ = "Select a file to preview first";
            }
        });

        auto btn_delete = Button("✕  Delete (x)", [&] {
            auto* selected = get_selected_file();
            if (selected) {
                delete_target_name = selected->name;
                modal_mode_ = ModalDeleteConfirm;
            } else {
                status_message_ = "Select a file to delete first";
            }
        });

        auto btn_refresh = Button("↻  Refresh (r)", [&] {
            refresh_files();
            status_message_ = std::format("✓ Refreshed vault index: {} files found", files_.size());
        });

        auto btn_help = Button("?  Help (?)", [&] {
            modal_mode_ = ModalHelp;
        });

        auto btn_quit = Button("⏻  Quit (q)", [&] {
            screen.Exit();
        });

        auto sidebar_container = Container::Vertical({
            btn_upload,
            btn_download,
            btn_preview,
            btn_delete,
            btn_refresh,
            btn_help,
            btn_quit
        });

        // ── Main Layout Container ──────────────────────────────────────────
        auto main_container = Container::Horizontal({
            sidebar_container,
            search_component
        });

        // ── Modal Renderer Components ──────────────────────────────────────
        auto upload_modal_renderer = Renderer(upload_container, [&] {
            auto progress_gauge = upload_running ? gauge(upload_progress) | color(Color::Green) : text("");
            return vbox({
                text("⬆  Upload File to Vault") | bold | color(Color::Cyan),
                separator(),
                text("Select local file to encrypt and upload:"),
                upload_input_path->Render() | borderLight,
                text("Encryption Password (optional):"),
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
                text("⬇  Download File from Vault") | bold | color(Color::Cyan),
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
            return vbox({
                text("✕  Confirm File Deletion") | bold | color(Color::Red),
                separator(),
                text(std::format("Are you sure you want to permanently delete '{}'?", delete_target_name)) | color(Color::White),
                text("This will delete the file entry and remove all chunk messages from Telegram.") | color(Color::GrayLight),
                separator(),
                Container::Horizontal({btn_delete_confirm, btn_delete_cancel})->Render()
            }) | borderRounded | size(WIDTH, GREATER_THAN, 55) | bgcolor(Color::RGB(32, 16, 16));
        });

        auto preview_modal_renderer = Renderer(btn_preview_close, [&] {
            return vbox({
                text(std::format("👁  Preview Chunk 0: {}", preview_title)) | bold | color(Color::Cyan),
                separator(),
                paragraph(preview_content) | color(Color::White) | size(HEIGHT, GREATER_THAN, 12) | size(HEIGHT, LESS_THAN, 22),
                separator(),
                btn_preview_close->Render()
            }) | borderRounded | size(WIDTH, GREATER_THAN, 70) | bgcolor(Color::RGB(20, 24, 32));
        });

        auto help_modal_renderer = Renderer(btn_help_close, [&] {
            return vbox({
                text("TeleVault TUI - Keyboard Shortcuts") | bold | color(Color::Cyan),
                separator(),
                hbox({text("  [↑ / k]     ") | bold | color(Color::Yellow), text("Navigate up in file list")}),
                hbox({text("  [↓ / j]     ") | bold | color(Color::Yellow), text("Navigate down in file list")}),
                hbox({text("  [PgUp/PgDn] ") | bold | color(Color::Yellow), text("Scroll 10 files up/down")}),
                hbox({text("  [Home/End]  ") | bold | color(Color::Yellow), text("Jump to first/last file")}),
                hbox({text("  [/]         ") | bold | color(Color::Yellow), text("Focus search filter box")}),
                hbox({text("  [u]         ") | bold | color(Color::Yellow), text("Upload local file to vault")}),
                hbox({text("  [d]         ") | bold | color(Color::Yellow), text("Download selected vault file")}),
                hbox({text("  [p]         ") | bold | color(Color::Yellow), text("Instant preview (chunk 0)")}),
                hbox({text("  [x / Del]   ") | bold | color(Color::Yellow), text("Delete selected vault file")}),
                hbox({text("  [r]         ") | bold | color(Color::Yellow), text("Refresh vault index from Telegram")}),
                hbox({text("  [Esc]       ") | bold | color(Color::Yellow), text("Close modal / clear search")}),
                hbox({text("  [q]         ") | bold | color(Color::Yellow), text("Quit TeleVault TUI")}),
                separator(),
                btn_help_close->Render()
            }) | borderRounded | size(WIDTH, GREATER_THAN, 60) | bgcolor(Color::RGB(20, 24, 32));
        });

        // ── Main View Renderer ─────────────────────────────────────────────
        auto main_renderer = Renderer(main_container, [&] {
            // Header Element
            auto header_elem = hbox({
                text(" 🛡️ TeleVault v3.5.0 ") | bold | color(Color::Cyan),
                text("│ Encrypted Cloud Storage via Telegram MTProto (C++23) ") | color(Color::GrayLight),
                filler(),
                text(" AES-256-GCM • Blake3 • Zstandard ") | color(Color::Green) | bold,
            }) | bgcolor(Color::RGB(16, 20, 28));

            // Sidebar Statistics Box
            auto stats_box = vbox({
                text(" Vault Statistics ") | bold | color(Color::Cyan),
                separator(),
                hbox({text("Total Files: ") | color(Color::GrayLight), text(std::to_string(files_.size())) | bold | color(Color::White)}),
                hbox({text("Vault Size:  ") | color(Color::GrayLight), text(format_file_size(total_bytes_)) | bold | color(Color::Green)}),
                hbox({text("Filtered:    ") | color(Color::GrayLight), text(std::to_string(filtered_indices_.size())) | bold | color(Color::Yellow)}),
                hbox({text("Encryption:  ") | color(Color::GrayLight), text("AES-256-GCM") | color(Color::Green)}),
                hbox({text("Integrity:   ") | color(Color::GrayLight), text("Blake3") | color(Color::Green)}),
            }) | borderLight;

            auto sidebar_elem = vbox({
                stats_box,
                separator(),
                text(" Actions ") | bold | color(Color::Cyan),
                btn_upload->Render(),
                btn_download->Render(),
                btn_preview->Render(),
                btn_delete->Render(),
                separator(),
                btn_refresh->Render(),
                btn_help->Render(),
                btn_quit->Render(),
                filler(),
            }) | size(WIDTH, EQUAL, 25) | bgcolor(Color::RGB(18, 22, 30));

            // File Table Elements
            Elements table_rows;
            table_rows.push_back(hbox({
                text("  ") | bold,
                text("Name") | bold | color(Color::Cyan) | flex,
                text("Size        ") | bold | color(Color::Cyan),
                text("Chunks  ") | bold | color(Color::Cyan),
                text("Encrypted  ") | bold | color(Color::Cyan),
                text("Created            ") | bold | color(Color::Cyan),
            }) | bgcolor(Color::RGB(24, 30, 42)));
            table_rows.push_back(separator());

            if (filtered_indices_.empty()) {
                table_rows.push_back(text("  (No files match your search criteria)") | color(Color::GrayLight));
            } else {
                int display_start = std::max(0, selected_index_ - 10);
                int display_end = std::min<int>(filtered_indices_.size(), display_start + 25);

                for (int i = display_start; i < display_end; ++i) {
                    int file_idx = filtered_indices_[i];
                    const auto& file = files_[file_idx];
                    bool is_selected = (i == selected_index_);

                    std::string icon = get_file_icon(file.name);
                    auto row = hbox({
                        text(is_selected ? "▶ " : "  ") | color(Color::Cyan),
                        text(icon + file.name) | flex,
                        text(std::format("{:>10}", format_file_size(file.size))),
                        text("  "),
                        text(std::format("{:>6}", file.chunk_count)),
                        text("  "),
                        text(file.encrypted ? "✓ AES-GCM " : "- Plain   ") | color(file.encrypted ? Color::Green : Color::GrayLight),
                        text(" "),
                        text(format_time_point(file.created_at)),
                    });

                    if (is_selected) {
                        row = row | inverted | bold;
                    }
                    table_rows.push_back(row);
                }
            }

            auto file_table_elem = vbox(std::move(table_rows)) | flex | borderLight;

            // Selected Item Detail Box
            Element detail_box;
            auto* selected_file = get_selected_file();
            if (selected_file) {
                std::string hash_display = selected_file->hash.empty() ? "(none)" : selected_file->hash;
                detail_box = vbox({
                    text(std::format("File Details: {}", selected_file->name)) | bold | color(Color::Cyan),
                    separator(),
                    hbox({
                        text("File ID: ") | color(Color::GrayLight),
                        text(selected_file->id) | color(Color::White),
                        text("  │  Blake3 Hash: ") | color(Color::GrayLight),
                        text(hash_display) | color(Color::Yellow),
                    }),
                    hbox({
                        text("Size: ") | color(Color::GrayLight),
                        text(format_file_size(selected_file->size)) | bold | color(Color::Green),
                        text("  │  Chunks: ") | color(Color::GrayLight),
                        text(std::to_string(selected_file->chunk_count)) | color(Color::White),
                        text("  │  Created: ") | color(Color::GrayLight),
                        text(format_time_point(selected_file->created_at)) | color(Color::White),
                    })
                }) | borderLight | size(HEIGHT, EQUAL, 5);
            } else {
                detail_box = vbox({
                    text("No file selected") | color(Color::GrayLight)
                }) | borderLight | size(HEIGHT, EQUAL, 3);
            }

            // Center Content
            auto content_elem = vbox({
                hbox({
                    text(" Filter: ") | bold | color(Color::Yellow),
                    search_component->Render() | flex,
                    text(std::format(" [{}/{} files] ", filtered_indices_.size(), files_.size())) | color(Color::GrayLight),
                }) | borderLight,
                file_table_elem,
                detail_box
            }) | flex;

            // Footer / Status Bar
            auto footer_elem = hbox({
                text(" ") | color(Color::Cyan),
                text(status_message_) | color(Color::White) | bold,
                filler(),
                text("[q] Quit  [r] Refresh  [/] Search  [u] Upload  [d] Download  [p] Preview  [x] Del  [?] Help ") | color(Color::GrayLight),
            }) | bgcolor(Color::RGB(16, 20, 28));

            return vbox({
                header_elem,
                hbox({
                    sidebar_elem,
                    content_elem
                }) | flex,
                footer_elem
            });
        });

        // ── Active Modal Wrapper ───────────────────────────────────────────
        auto modal_container = Container::Tab({
            main_renderer,
            upload_modal_renderer,
            download_modal_renderer,
            preview_modal_renderer,
            delete_modal_renderer,
            help_modal_renderer,
        }, &modal_mode_);

        // ── Keyboard Navigation & Global Shortcuts ─────────────────────────
        auto top_component = CatchEvent(modal_container, [&](Event event) {
            // If in modal, allow Esc to close modal
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
            if (event == Event::Character('r') || event == Event::Character('R') || event == Event::F5) {
                refresh_files();
                status_message_ = std::format("✓ Refreshed vault index ({} files)", files_.size());
                return true;
            }
            if (event == Event::Character('?') || event == Event::F1) {
                modal_mode_ = ModalHelp;
                return true;
            }
            if (event == Event::Character('/')) {
                search_component->TakeFocus();
                return true;
            }
            if (event == Event::Escape) {
                if (!search_query_.empty()) {
                    search_query_.clear();
                    apply_filter();
                    return true;
                }
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
                    status_message_ = "Select a file to download first";
                }
                return true;
            }
            if (event == Event::Character('p') || event == Event::Character('P')) {
                btn_preview->OnEvent(Event::Return);
                return true;
            }
            if (event == Event::Character('x') || event == Event::Character('X') || event == Event::Delete) {
                auto* selected = get_selected_file();
                if (selected) {
                    delete_target_name = selected->name;
                    modal_mode_ = ModalDeleteConfirm;
                }
                return true;
            }

            // Arrow keys & j/k navigation for file list
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                if (!filtered_indices_.empty() && selected_index_ > 0) {
                    --selected_index_;
                    return true;
                }
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                if (!filtered_indices_.empty() && selected_index_ + 1 < static_cast<int>(filtered_indices_.size())) {
                    ++selected_index_;
                    return true;
                }
            }
            if (event == Event::PageUp) {
                selected_index_ = std::max(0, selected_index_ - 10);
                return true;
            }
            if (event == Event::PageDown) {
                if (!filtered_indices_.empty()) {
                    selected_index_ = std::min<int>(filtered_indices_.size() - 1, selected_index_ + 10);
                }
                return true;
            }
            if (event == Event::Home) {
                selected_index_ = 0;
                return true;
            }
            if (event == Event::End) {
                if (!filtered_indices_.empty()) {
                    selected_index_ = static_cast<int>(filtered_indices_.size()) - 1;
                }
                return true;
            }

            return false;
        });

        screen.Loop(top_component);
    }

private:
    void refresh_files() {
        files_ = vault_.list_files();
        total_bytes_ = 0;
        for (const auto& file : files_) {
            total_bytes_ += file.size;
        }
        apply_filter();
    }

    void apply_filter() {
        filtered_indices_ = tui::filter_file_indices(files_, search_query_);

        if (filtered_indices_.empty()) {
            selected_index_ = 0;
        } else if (selected_index_ >= static_cast<int>(filtered_indices_.size())) {
            selected_index_ = static_cast<int>(filtered_indices_.size()) - 1;
        }
    }

    const FileEntry* get_selected_file() const {
        if (filtered_indices_.empty() || selected_index_ < 0 || selected_index_ >= static_cast<int>(filtered_indices_.size())) {
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
    std::vector<FileEntry> files_;
    std::vector<int> filtered_indices_;
    int selected_index_{0};
    uint64_t total_bytes_{0};
    std::string search_query_;
    std::string status_message_{"Ready"};
    int modal_mode_{ModalNone};
};

TUI::TUI(TeleVault& vault) : impl_(std::make_unique<Impl>(vault)) {}
TUI::~TUI() = default;

void TUI::run() {
    if (impl_) {
        impl_->run();
    }
}

} // namespace tv
