#include "cli.hpp"
#include "progress.hpp"
#include "../core/app_context.hpp"
#include "../telegram/client.hpp"
#include "../telegram/auth.hpp"
#include "../telegram/session.hpp"
#include "../core/vault.hpp"
#include "../util/config.hpp"
#include "../util/format.hpp"
#include "../preview/preview.hpp"
#include "../backup/engine.hpp"
#include "../watcher/watcher.hpp"
#include "../schedule/schedule.hpp"
#include "../gc/gc.hpp"
#if defined(TV_BUILD_TUI)
#include "../tui/tui.hpp"
#endif

#include <iostream>
#include <print>
#include <format>
#include <string>
#include <fstream>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <spdlog/spdlog.h>
#include "../webdav/stream_server.hpp"

#if defined(TV_BUILD_FUSE)
#include "../fuse/fuse_ops.hpp"
#endif

#if defined(TV_BUILD_WEBDAV)
#include "../webdav/server.hpp"
#include "../webdav/s3_server.hpp"
#include "../webdav/share_server.hpp"
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace tv {

namespace {

    int get_terminal_width() {
        int cols = 80;
#if defined(__unix__) || defined(__APPLE__)
        struct winsize w{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
            cols = w.ws_col;
        } else
#endif
        {
            const char* env_cols = std::getenv("COLUMNS");
            if (env_cols) {
                try { cols = std::stoi(env_cols); } catch (...) {}
            }
        }
        return std::max(cols, 60);
    }

    // ── Helpers ──────────────────────────────────────────────────────
    void print_success(const std::string& msg) {
        std::println("\033[32m✓ {}\033[0m", msg);
    }

    void print_error(const std::string& msg) {
        std::println("\033[31m✗ {}\033[0m", msg);
    }

    void print_info(const std::string& msg) {
        std::println("{}", msg);
    }

    // ── Auth commands ───────────────────────────────────────────────
    void cmd_login(AppContext& ctx, bool use_qr) {
        SessionManager sm;

        if (sm.has_api_credentials()) {
            // Already have credentials from config — push to client
            ctx.tg_client.set_api_params(sm.api_id(), sm.api_hash());
            if (ctx.tg_client.connect() && ctx.tg_client.is_authorized()) {
                print_info("Already logged in. Use 'logout' first to re-authenticate.");
                return;
            }
        } else {
            std::print("Enter API ID (from my.telegram.org): ");
            std::string api_id_str;
            std::getline(std::cin, api_id_str);

            std::print("Enter API Hash: ");
            std::string api_hash;
            std::getline(std::cin, api_hash);

            int32_t api_id = 0;
            try {
                api_id = std::stoi(api_id_str);
            } catch (...) {}
            if (api_id <= 0 || api_hash.empty()) {
                print_error("Invalid API credentials");
                return;
            }

            ctx.tg_client.set_api_params(api_id, api_hash);
            sm.save_api_credentials(api_id, api_hash, {});
        }

        std::print("Enter phone number (with country code): ");
        std::string phone;
        if (!use_qr) {
            std::getline(std::cin, phone);

            if (phone.empty()) {
                print_error("Phone number cannot be empty");
                return;
            }
        }

        AuthFlow auth(ctx.tg_client);

        auto code_cb = []() -> std::string {
            std::print("Enter the code you received: ");
            std::string code;
            std::getline(std::cin, code);
            return code;
        };

        auto pw_cb = []() -> std::string {
            std::print("Enter your 2FA password: ");
            std::string pw;
            std::getline(std::cin, pw);
            return pw;
        };

        if (use_qr) {
            print_info("QR login: scan the code below with Telegram (Settings → Devices → Link Desktop Device).");
            auto state = auth.execute_qr(pw_cb);
            if (state == AuthFlow::State::Done) {
                print_success("Successfully authenticated as " + ctx.tg_client.get_my_username());
            } else {
                print_error("QR authentication failed (code expired or not confirmed in time).");
            }
            return;
        }

        auto state = auth.execute(phone, code_cb, pw_cb);
        if (state == AuthFlow::State::Done) {
            print_success("Successfully authenticated as " + ctx.tg_client.get_my_username());
        } else {
            print_error("Authentication failed");
        }
    }

    void cmd_logout(AppContext& ctx) {
        AuthFlow auth(ctx.tg_client);
        auth.logout();
        SessionManager sm;
        sm.clear();
        print_success("Logged out");
    }

    void ensure_vault(AppContext& ctx);

    void cmd_setup(AppContext& ctx) {
        std::print("Enter Telegram channel ID (or 0 to create new): ");
        std::string input;
        std::getline(std::cin, input);
        int64_t channel_id = input.empty() ? 0 : std::stoll(input);

        if (channel_id == 0) {
            std::print("Enter channel name: ");
            std::string name;
            std::getline(std::cin, name);

            if (!ctx.tg_client.connect()) {
                print_error("Not connected. Please login first.");
                return;
            }

            if (!ctx.tg_client.create_channel(name)) {
                print_error("Failed to create channel");
                return;
            }
            channel_id = ctx.tg_client.get_channel_id();
            print_success(std::format("Created channel with ID: {}", channel_id));
        } else {
            if (!ctx.tg_client.connect()) {
                print_error("Not connected");
                return;
            }
            if (!ctx.tg_client.is_valid_channel(channel_id)) {
                print_error("Invalid channel ID or bot not in channel");
                return;
            }
            ctx.tg_client.set_channel(channel_id);
            print_success("Channel validated");
        }

        auto& cfg = ConfigManager::instance();
        auto config = cfg.get();
        config.channel_id = channel_id;
        cfg.set(config);
        cfg.save();

        try {
            print_info("Synchronizing vault index with channel...");
            ensure_vault(ctx);
            ctx.vault->sync(true);
            auto files = ctx.vault->list_files();
            print_success(std::format("Vault synchronized: {} file(s) available", files.size()));
        } catch (const std::exception& e) {
            print_info(std::format("Vault sync note: {}", e.what()));
        }
    }

    void cmd_channel(AppContext& ctx) {
        auto& cfg = ConfigManager::instance();
        auto channel_id = cfg.get().channel_id;
        if (channel_id == 0) {
            print_info("No channel configured. Use 'setup' first.");
        } else {
            print_info(std::format("Channel ID: {}", channel_id));
        }
    }

    void cmd_whoami(AppContext& ctx) {
        if (!ctx.tg_client.connect() || !ctx.tg_client.is_authorized()) {
            print_error("Not authenticated. Use 'login' first.");
            return;
        }
        auto username = ctx.tg_client.get_my_username();
        auto user_id = ctx.tg_client.get_my_id();
        print_info(std::format("User: @{} (ID: {})", username, user_id));
    }

    void ensure_vault(AppContext& ctx) {
        if (!ctx.tg_client.connect() || !ctx.tg_client.is_authorized()) {
            throw std::runtime_error("Not authenticated. Use 'login' first.");
        }
        auto& cfg = ConfigManager::instance().get();
        if (cfg.channel_id == 0) {
            throw std::runtime_error("No channel configured. Use 'setup' first.");
        }
        if (!ctx.ensure_vault()) {
            throw std::runtime_error("Vault channel initialization failed. Check channel access or run 'setup'.");
        }
    }

    std::string resolve_password(const std::string& explicit_pw, bool prompt_if_empty = true) {
        if (!explicit_pw.empty()) return explicit_pw;
        if (const char* env_pw = std::getenv("TELEVAULT_PASSWORD")) {
            if (std::string(env_pw).length() > 0) return std::string(env_pw);
        }
        if (prompt_if_empty) {
            std::print("Enter encryption password: ");
            std::string pw;
            std::getline(std::cin, pw);
            return pw;
        }
        return {};
    }

    void cmd_push(AppContext& ctx, const std::string& path, const std::string& password,
                  bool recursive, bool resume, bool low_resource, bool no_encryption,
                  bool delta = false, const std::vector<int64_t>& shards = {}) {
        ensure_vault(ctx);
        VaultOptions opts;
        opts.low_resource = low_resource;
        opts.resume = resume;
        opts.delta = delta;
        opts.channel_ids = shards;

        if (!shards.empty()) {
            ctx.vault->set_shards(shards);
        }

        auto& cfg = ConfigManager::instance().get();
        opts.encrypted = !no_encryption && cfg.encryption;
        opts.compressed = cfg.compression;

        if (opts.encrypted) {
            opts.password = resolve_password(password);
            if (opts.password.empty()) {
                print_error("Password cannot be empty when encryption is enabled");
                return;
            }
        }

        ProgressBar pb;
        auto cb = [&pb](const ProgressInfo& p) {
            if (!p.stage.empty()) {
                pb.set_message(p.stage);
            }
            if (p.total > 0) {
                pb.update(p.current, p.total);
            }
        };

        bool ok = ctx.vault->push(path, opts, cb);
        pb.finish();
        if (ok) print_success(std::format("Uploaded: {}", path));
        else print_error(std::format("Upload failed: {}", path));
    }

    void cmd_pull(AppContext& ctx, const std::string& query, const std::string& output,
                  const std::string& password, bool resume, bool low_resource) {
        ensure_vault(ctx);
        VaultOptions opts;
        opts.low_resource = low_resource;
        opts.resume = resume;

        auto& cfg = ConfigManager::instance().get();
        opts.encrypted = cfg.encryption;

        if (opts.encrypted) {
            opts.password = resolve_password(password);
            if (opts.password.empty()) {
                print_error("Password cannot be empty when encryption is enabled");
                return;
            }
        }

        std::string target_file;

        if (query.empty()) {
            // Interactive mode: list all files and let user choose
            auto all_files = ctx.vault->list_files();
            if (all_files.empty()) {
                print_error("Vault is empty. Nothing to pull.");
                return;
            }
            std::println("\033[1mFiles in vault:\033[0m");
            for (size_t i = 0; i < all_files.size(); ++i) {
                std::println("  [{}] {:<35} ({})", i + 1, all_files[i].name, format_size(all_files[i].size));
            }
            std::print("\033[1;36mSelect file to download [1-{}] (or 'q' to cancel): \033[0m", all_files.size());
            std::string selection;
            if (!std::getline(std::cin, selection) || selection == "q" || selection == "Q" || selection.empty()) {
                print_info("Cancelled.");
                return;
            }
            try {
                size_t idx = std::stoul(selection);
                if (idx < 1 || idx > all_files.size()) {
                    print_error("Invalid selection.");
                    return;
                }
                target_file = all_files[idx - 1].name;
            } catch (...) {
                print_error("Invalid input.");
                return;
            }
        } else {
            // Check for matching files
            auto matches = ctx.vault->find_all_matching(query);
            if (matches.empty()) {
                print_error(std::format("File not found matching: '{}'", query));
                return;
            }
            if (matches.size() == 1) {
                target_file = matches[0].name;
            } else {
                // Check if one is an exact match
                bool found_exact = false;
                for (const auto& m : matches) {
                    if (m.name == query || m.id == query) {
                        target_file = m.name;
                        found_exact = true;
                        break;
                    }
                }
                if (!found_exact) {
                    std::println("\033[1mMultiple files matched '{}':\033[0m", query);
                    for (size_t i = 0; i < matches.size(); ++i) {
                        std::println("  [{}] {:<35} ({})", i + 1, matches[i].name, format_size(matches[i].size));
                    }
                    std::print("\033[1;36mSelect file to download [1-{}] (or 'q' to cancel): \033[0m", matches.size());
                    std::string selection;
                    if (!std::getline(std::cin, selection) || selection == "q" || selection == "Q" || selection.empty()) {
                        print_info("Cancelled.");
                        return;
                    }
                    try {
                        size_t idx = std::stoul(selection);
                        if (idx < 1 || idx > matches.size()) {
                            print_error("Invalid selection.");
                            return;
                        }
                        target_file = matches[idx - 1].name;
                    } catch (...) {
                        print_error("Invalid input.");
                        return;
                    }
                }
            }
        }

        auto output_path = output.empty() ? target_file : output;

        ProgressBar pb;
        auto cb = [&pb](const ProgressInfo& p) {
            if (!p.stage.empty()) {
                pb.set_message(p.stage);
            }
            if (p.total > 0) {
                pb.update(p.current, p.total);
            }
        };

        bool ok = ctx.vault->pull(target_file, output_path, opts, cb);
        pb.finish();
        if (ok) print_success(std::format("Downloaded: {} → {}", target_file, output_path));
        else print_error(std::format("Download failed: {}", target_file));
    }

    void cmd_ls(AppContext& ctx, bool json_output, const std::string& sort, bool wide) {
        ensure_vault(ctx);
        auto files = ctx.vault->list_files();

        if (!sort.empty()) {
            if (sort == "name") {
                std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
                    return a.name < b.name;
                });
            } else if (sort == "size") {
                std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
                    return a.size > b.size;
                });
            }
        }

        if (json_output) {
            std::println("[");
            for (size_t i = 0; i < files.size(); ++i) {
                auto& f = files[i];
                std::println("  {{\"id\":\"{}\",\"name\":\"{}\",\"size\":{}}}",
                             f.id, f.name, f.size);
                if (i < files.size() - 1) std::println(",");
            }
            std::println("]");
            return;
        }

        int term_width = get_terminal_width();
        int name_width = 40;
        if (wide) {
            size_t max_name = 40;
            for (const auto& f : files) {
                max_name = std::max(max_name, f.name.size());
            }
            name_width = static_cast<int>(max_name) + 2;
        } else {
            name_width = std::max(25, term_width - 36);
        }

        std::string hdr_name = "Name";
        if (static_cast<int>(hdr_name.size()) < name_width) {
            hdr_name.append(name_width - hdr_name.size(), ' ');
        }
        std::string hdr_sep(name_width, '-');

        std::println("{:<18} {} {:>12}", "ID", hdr_name, "Size");
        std::println("{:-<18} {} {:->12}", "", hdr_sep, "");
        for (auto& f : files) {
            auto short_id = f.id.substr(0, 16);
            std::string disp_name = f.name;
            if (!wide && static_cast<int>(disp_name.size()) > name_width) {
                disp_name = disp_name.substr(0, name_width - 3) + "...";
            }
            if (static_cast<int>(disp_name.size()) < name_width) {
                disp_name.append(name_width - disp_name.size(), ' ');
            }
            std::println("{:<18} {} {:>12}", short_id, disp_name, format_size(f.size));
        }
        std::println("\nTotal: {} files", files.size());
    }

    void cmd_cat(AppContext& ctx, const std::string& path, const std::string& password) {
        ensure_vault(ctx);
        VaultOptions opts;
        auto& cfg = ConfigManager::instance().get();
        opts.encrypted = cfg.encryption;

        if (opts.encrypted) {
            opts.password = resolve_password(password);
            if (opts.password.empty()) {
                print_error("Password cannot be empty when encryption is enabled");
                return;
            }
        }
        if (!ctx.vault->cat(path, opts)) {
            print_error("Failed to cat file");
        }
    }

    void cmd_find(AppContext& ctx, const std::string& query, bool json_output,
                  const std::string& ext, int64_t min_size, int64_t max_size) {
        ensure_vault(ctx);
        auto all_files = ctx.vault->list_files();
        std::vector<FileEntry> results;

        std::string query_lower = query;
        std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        std::string filter_ext = ext;
        if (!filter_ext.empty() && filter_ext[0] != '.') {
            filter_ext = "." + filter_ext;
        }
        std::transform(filter_ext.begin(), filter_ext.end(), filter_ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        for (const auto& f : all_files) {
            // Check size filter
            if (min_size >= 0 && static_cast<int64_t>(f.size) < min_size) continue;
            if (max_size >= 0 && static_cast<int64_t>(f.size) > max_size) continue;

            // Check extension filter
            if (!filter_ext.empty()) {
                auto file_ext = std::filesystem::path(f.name).extension().string();
                std::transform(file_ext.begin(), file_ext.end(), file_ext.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (file_ext != filter_ext) continue;
            }

            // Check query
            if (!query.empty()) {
                std::string fname_lower = f.name;
                std::transform(fname_lower.begin(), fname_lower.end(), fname_lower.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (fname_lower.find(query_lower) == std::string::npos && f.id.find(query) == std::string::npos) {
                    continue;
                }
            }

            results.push_back(f);
        }

        if (json_output) {
            std::println("[");
            for (size_t i = 0; i < results.size(); ++i) {
                auto& f = results[i];
                std::println("  {{\"id\":\"{}\",\"name\":\"{}\",\"size\":{}}}", f.id, f.name, f.size);
                if (i < results.size() - 1) std::println(",");
            }
            std::println("]");
            return;
        }

        if (results.empty()) {
            print_info("No files matched search criteria.");
            return;
        }

        std::println("Found {} matching file(s):", results.size());
        for (const auto& f : results) {
            std::string disp = f.name;
            if (!query.empty()) {
                std::string lower = disp;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                auto pos = lower.find(query_lower);
                if (pos != std::string::npos) {
                    disp = disp.substr(0, pos) + "\033[1;33m" + disp.substr(pos, query.size()) + "\033[0m" + disp.substr(pos + query.size());
                }
            }
            std::println("  {} ({})", disp, format_size(f.size));
        }
    }

    void cmd_stream(AppContext& ctx, const std::string& path, uint16_t port, const std::string& password) {
        ensure_vault(ctx);
        auto meta = ctx.vault->get_file_info(path);
        if (!meta) {
            print_error("File not found in vault: " + path);
            return;
        }

        StreamOptions s_opts;
        s_opts.host = "127.0.0.1";
        s_opts.port = port;
        s_opts.password = resolve_password(password);
        if (meta->encrypted && s_opts.password.empty()) {
            print_error("Password required to stream encrypted file");
            return;
        }

        // Preflight: decrypt 1 byte before binding the port. A wrong
        // password or corrupted chunk previously crashed the server with
        // an uncaught std::runtime_error (SIGABRT) on the first HTTP
        // request; fail fast here with a clear message instead.
        {
            VaultOptions vopts;
            vopts.password = s_opts.password;
            auto probe = ctx.vault->read_byte_range(meta->name, 0, 0, vopts);
            if (!probe) {
                if (meta->encrypted) {
                    print_error("Cannot decrypt '" + meta->name +
                                "': wrong password or corrupted chunk. "
                                "Verify with 'tvt pull' before streaming.");
                } else {
                    print_error("Cannot read '" + meta->name +
                                "' from vault (corrupted chunk). "
                                "Verify with 'tvt pull' before streaming.");
                }
                return;
            }
        }

        StreamServer server(*ctx.vault);
        std::println("\033[1;32m=== TeleVault Media Stream Server ===\033[0m");
        std::println("Streaming:   \033[1m{}\033[0m ({})", meta->name, format_size(meta->size));
        std::println("Stream URL:  \033[1;34m{}\033[0m", server.stream_url(meta->name));
        std::println("\nPlay in VLC, MPV, or your browser:");
        std::println("  \033[36mvlc {}\033[0m", server.stream_url(meta->name));
        std::println("  \033[36mmpv {}\033[0m\n", server.stream_url(meta->name));
        std::println("Press Ctrl+C to stop streaming.\n");

        if (!server.start(s_opts, true)) {
            print_error("Failed to start streaming server on port " + std::to_string(port));
        }
    }

    void cmd_completion(const std::string& shell) {
        if (shell == "bash") {
            std::cout << R"BASH(# TeleVault Bash Completion
_televault() {
    local cur prev words cword
    _init_completion || return

    local commands="login logout setup whoami push pull ls cat find search info stat rm verify recover gc tui preview stream mount serve serve-s3 share trash restore versions backup schedule watch completion"

    if [[ $cword -eq 1 ]]; then
        COMPREPLY=( $(compgen -W "$commands" -- "$cur") )
        return 0
    fi

    local subcmd="${words[1]}"
    case "$subcmd" in
        pull|cat|info|rm|verify|preview|stream|share|versions|restore)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "-p --password -o --output" -- "$cur") )
            else
                local files
                files=$(tvt __complete_files 2>/dev/null)
                COMPREPLY=( $(compgen -W "$files" -- "$cur") )
            fi
            ;;
        push)
            _filedir
            ;;
        trash)
            COMPREPLY=( $(compgen -W "list empty" -- "$cur") )
            ;;
        ls)
            COMPREPLY=( $(compgen -W "--json --sort -w --wide" -- "$cur") )
            ;;
        completion)
            COMPREPLY=( $(compgen -W "bash zsh fish" -- "$cur") )
            ;;
        *)
            ;;
    esac
}
complete -F _televault tvt televault
)BASH" << std::endl;
        } else if (shell == "zsh") {
            std::cout << R"ZSH(#compdef tvt televault
# TeleVault Zsh Completion

_televault() {
    local -a commands
    commands=(
        'login:Authenticate with Telegram'
        'logout:Log out and clear session'
        'setup:Select or create storage channel'
        'whoami:Display current user'
        'push:Upload a file or directory'
        'pull:Download a file from vault'
        'ls:List files in vault'
        'cat:Stream file content to stdout'
        'find:Search files by name or pattern'
        'search:Search files by name or pattern'
        'info:Display detailed file info'
        'stat:Display vault statistics'
        'rm:Delete a file from vault'
        'verify:Verify file integrity'
        'recover:Recover vault index from channel history'
        'gc:Garbage collection'
        'tui:Launch terminal user interface'
        'preview:Preview a file'
        'stream:Stream media file with HTTP Range'
        'mount:Mount FUSE filesystem'
        'serve:Start WebDAV server'
        'serve-s3:Start S3-compatible storage gateway'
        'share:Generate ephemeral direct share link'
        'trash:Encrypted trash bin management'
        'restore:Restore file from encrypted trash'
        'versions:List file version history'
        'backup:Snapshot backup management'
        'schedule:Backup scheduling'
        'watch:Watch directory for changes'
        'completion:Generate shell autocompletion'
    )

    _arguments -C \
        '1: :->command' \
        '*: :->args'

    case $state in
        command)
            _describe -t commands 'televault commands' commands
            ;;
        args)
            case $words[2] in
                pull|cat|info|rm|verify|preview|stream|share|versions|restore)
                    local -a vfiles
                    vfiles=(${(f)"$(tvt __complete_files 2>/dev/null)"})
                    _describe 'vault files' vfiles
                    ;;
                push)
                    _files
                    ;;
                trash)
                    _values 'subcmd' list empty
                    ;;
                ls)
                    _arguments \
                        '--json[Output JSON]' \
                        '(-w --wide)'{-w,--wide}'[Disable name truncation]' \
                        '--sort[Sort field]:field:(name size)'
                    ;;
                completion)
                    _values 'shell' bash zsh fish
                    ;;
            esac
            ;;
    esac
}

_televault "$@"
)ZSH" << std::endl;
        } else if (shell == "fish") {
            std::cout << R"FISH(# TeleVault Fish Completion
set -l commands login logout setup whoami push pull ls cat find search info stat rm verify recover gc tui preview stream mount serve serve-s3 share trash restore versions backup schedule watch completion

complete -c tvt -f -n "not __fish_seen_subcommand_from $commands" -a "$commands"
complete -c televault -f -n "not __fish_seen_subcommand_from $commands" -a "$commands"

function __tvt_vault_files
    tvt __complete_files 2>/dev/null
end

for cmd in pull cat info rm verify preview stream
    complete -c tvt -f -n "__fish_seen_subcommand_from $cmd" -a "(__tvt_vault_files)"
    complete -c televault -f -n "__fish_seen_subcommand_from $cmd" -a "(__tvt_vault_files)"
end

complete -c tvt -n "__fish_seen_subcommand_from completion" -a "bash zsh fish"
complete -c televault -n "__fish_seen_subcommand_from completion" -a "bash zsh fish"
complete -c tvt -n "__fish_seen_subcommand_from ls" -l json -d "JSON output"
complete -c tvt -n "__fish_seen_subcommand_from ls" -s w -l wide -d "Disable truncation"
)FISH" << std::endl;
        } else {
            print_error("Unsupported shell: " + shell + ". Supported: bash, zsh, fish");
        }
    }

    void cmd_complete_files(AppContext& ctx) {
        if (!ctx.ensure_vault()) return;
        auto files = ctx.vault->list_files();
        for (const auto& f : files) {
            std::println("{}", f.name);
        }
    }

    void cmd_info(AppContext& ctx, const std::string& path, bool json_output) {
        ensure_vault(ctx);
        auto info = ctx.vault->get_file_info(path);
        if (!info) {
            print_error("File not found: " + path);
            return;
        }

        if (json_output) {
            nlohmann::json j = *info;
            std::println("{}", j.dump(2));
        } else {
            std::println("Name:      {}", info->name);
            std::println("Size:      {}", format_size(info->size));
            std::println("Hash:      {}", info->hash);
            std::println("Encrypted: {}", info->encrypted ? "yes" : "no");
            std::println("Compressed:{}", info->compressed ? "yes" : "no");
            std::println("Chunks:    {}", info->chunks.size());
        }
    }

    void cmd_stat(AppContext& ctx, bool json_output) {
        ensure_vault(ctx);
        auto files = ctx.vault->list_files();

        uint64_t total_size{};
        for (auto& f : files) total_size += f.size;

        if (json_output) {
            std::println("{{\"files\":{},\"total_size\":{}}}", files.size(), total_size);
        } else {
            print_info(std::format("Files: {}\nTotal size: {}",
                                   files.size(), format_size(total_size)));
        }
    }

    void cmd_rm(AppContext& ctx, const std::string& path, bool purge = false) {
        ensure_vault(ctx);
        if (ctx.vault->delete_file(path, purge)) {
            if (purge) {
                print_success("Permanently purged: " + path);
            } else {
                print_success("Moved to encrypted trash: " + path + " (use 'tvt restore " + path + "' to recover)");
            }
        } else {
            print_error("Delete failed: " + path);
        }
    }

    void cmd_verify(AppContext& ctx, const std::string& path) {
        ensure_vault(ctx);
        if (ctx.vault->verify_file(path)) {
            print_success("Integrity verified: " + path);
        } else {
            print_error("Integrity check FAILED: " + path);
        }
    }

    void cmd_recover(AppContext& ctx) {
        ensure_vault(ctx);
        print_info("Scanning Telegram channel for file metadata...");
        if (ctx.vault->recover_index()) {
            print_success("Vault index successfully recovered and pinned!");
        } else {
            print_error("Recovery failed: no file metadata found in channel history");
        }
    }

    void cmd_sync(AppContext& ctx) {
        ensure_vault(ctx);
        print_info("Synchronizing vault index with Telegram channel...");
        if (ctx.vault->sync(true)) {
            auto files = ctx.vault->list_files();
            print_success(std::format("Sync complete: {} file(s) available in vault", files.size()));
        } else {
            print_error("Failed to synchronize with Telegram channel");
        }
    }

    // ── GC ────────────────────────────────────────────────────────────
    void cmd_gc(AppContext& ctx, bool force, bool clean_partials) {
        ensure_vault(ctx);
        auto& cfg = ConfigManager::instance().get();
        if (cfg.channel_id == 0) {
            print_error("No channel configured. Use 'setup' first.");
            return;
        }
        bool dry_run = !force;
        print_info(std::format("Scanning channel for orphans (dry-run: {})...", dry_run ? "yes" : "NO — deleting"));

        auto res = collect_garbage(ctx.tg_client, *ctx.vault, cfg.channel_id, dry_run);
        std::println("Scanned messages: {}", res.scanned_messages);
        if (res.orphans.empty()) {
            print_success("No orphaned messages found.");
        } else {
            std::println("{:<20} {:<16}", "MESSAGE ID", "TYPE");
            std::println("{:-<20} {:-<16}", "", "");
            for (auto& o : res.orphans) {
                std::println("{:<20} {:<16}", o.message_id, o.type);
            }
            if (dry_run) {
                print_info("Re-run with --force to delete these orphans (snapshots are never auto-deleted).");
            } else {
                print_success(std::format("GC complete: {} orphan(s) processed.", res.orphans.size()));
            }
        }

        if (clean_partials) {
            auto partials = cleanup_partial_uploads(ctx.tg_client, *ctx.vault, cfg.channel_id, dry_run);
            if (partials.empty()) {
                print_success("No partial uploads found.");
            } else if (dry_run) {
                print_info("Re-run with --force to drop these partial index entries.");
            }
        }
    }

    // ── TUI ───────────────────────────────────────────────────────────
    void cmd_tui(AppContext& ctx) {
#if defined(TV_BUILD_TUI) && defined(TV_BUILD_TDLIB)
        if (!ctx.ensure_vault()) {
            print_error("Failed to initialize vault for TUI mode. Ensure you are logged in (tvt login) and channel is configured (tvt setup).");
            return;
        }
        try {
            TUI tui(*ctx.vault);
            tui.run();
        } catch (const std::exception& e) {
            print_error(std::format("TUI terminated with error: {}", e.what()));
        }
#else
        print_error("TeleVault was built without TUI support. Rebuild with -DTV_BUILD_TUI=ON -DTV_BUILD_TDLIB=ON.");
#endif
    }

    // ── Preview ───────────────────────────────────────────────────────
    void cmd_preview(AppContext& ctx, const std::string& path, const std::string& password) {
        // 1. If path is a local file, preview directly
        if (std::filesystem::exists(path) && std::filesystem::is_regular_file(path)) {
            PreviewEngine engine;
            auto res = engine.preview(path);
            std::println("\033[1;34m=== Preview: {} ===\033[0m", path);
            std::println("Size:      {} ({} bytes)", format_size(std::filesystem::file_size(path)), std::filesystem::file_size(path));
            std::println("MIME type: {}", res.mime_type);
            std::println("----------------------------------------");
            std::println("{}", res.text_preview);
            return;
        }

        // 2. Otherwise preview from vault
        ensure_vault(ctx);
        auto meta = ctx.vault->get_file_info(path);
        if (!meta) {
            print_error("File not found in vault: " + path);
            return;
        }

        VaultOptions opts;
        opts.encrypted = meta->encrypted;
        if (opts.encrypted) {
            opts.password = resolve_password(password);
            if (opts.password.empty()) {
                print_error("Password required to preview encrypted file");
                return;
            }
        }
        opts.compressed = meta->compressed;

        auto chunk0_data = ctx.vault->read_first_chunk(path, opts);
        if (!chunk0_data || chunk0_data->empty()) {
            print_error("Failed to fetch chunk for preview");
            return;
        }

        auto tmp = std::filesystem::temp_directory_path() /
            std::format("tvt_prev_{}_{}", meta->id, std::rand());
        {
            std::ofstream f(tmp, std::ios::binary);
            f.write(reinterpret_cast<const char*>(chunk0_data->data()), chunk0_data->size());
        }

        PreviewEngine engine;
        auto res = engine.preview(tmp.string(), meta->name);
        std::filesystem::remove(tmp);

        std::println("\033[1;34m=== Preview: {} ===\033[0m", meta->name);
        std::println("Size:      {} ({} bytes)", format_size(meta->size), meta->size);
        std::println("MIME type: {}", res.mime_type);
        std::println("Encrypted: {}", meta->encrypted ? "yes" : "no");
        std::println("Compressed:{}", meta->compressed ? "yes" : "no");
        std::println("----------------------------------------");
        std::println("{}", res.text_preview);
    }

    // ── Backup ────────────────────────────────────────────────────────
    void cmd_backup_create(AppContext& ctx, const std::vector<std::string>& paths,
                           const std::string& name, const std::string& password, bool incremental) {
        ensure_vault(ctx);
        if (paths.empty()) {
            print_error("At least one path must be specified for backup");
            return;
        }
        auto& cfg = ConfigManager::instance().get();
        std::string pass = password;
        if (cfg.encryption && pass.empty()) {
            pass = resolve_password("");
            if (pass.empty()) {
                print_error("Password cannot be empty when encryption is enabled");
                return;
            }
        }

        BackupEngine engine(*ctx.vault, ctx.tg_client);
        std::string snap_name = name.empty() ? std::format("backup_{}", std::filesystem::path(paths[0]).filename().string()) : name;
        print_info("Creating snapshot: " + snap_name);

        ProgressBar bar;
        bool ok = engine.create_snapshot(snap_name, paths, pass, incremental,
            [&bar](const ProgressInfo& p) {
                bar.update(p.current, p.total, p.stage);
            });
        bar.finish();
        if (ok) {
            print_success("Snapshot created successfully!");
        } else {
            print_error("Failed to create snapshot");
        }
    }

    void cmd_backup_list(AppContext& ctx) {
        ensure_vault(ctx);
        BackupEngine engine(*ctx.vault, ctx.tg_client);
        auto snapshots = engine.list_snapshots();
        if (snapshots.empty()) {
            std::println("No snapshots found in vault.");
            return;
        }

        std::println("{:<16} {:<24} {:<8} {:<12} {:<16}",
                     "Snapshot ID", "Name", "Files", "Size", "Created");
        std::println("---------------- ------------------------ -------- ------------ ----------------");
        for (const auto& s : snapshots) {
            auto t = std::chrono::system_clock::to_time_t(s.created_at);
            std::tm tm_buf{};
            localtime_r(&t, &tm_buf);
            char date_str[32];
            std::strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M", &tm_buf);
            std::println("{:<16} {:<24} {:<8} {:<12} {:<16}",
                         s.id, s.name, s.file_count, format_size(s.total_size), date_str);
        }
        std::println("\nTotal: {} snapshot(s)", snapshots.size());
    }

    void cmd_backup_restore(AppContext& ctx, const std::string& snapshot_id,
                            const std::string& output_dir, const std::string& password) {
        ensure_vault(ctx);
        auto& cfg = ConfigManager::instance().get();
        std::string pass = password;
        if (cfg.encryption && pass.empty()) {
            pass = resolve_password("");
            if (pass.empty()) {
                print_error("Password cannot be empty when encryption is enabled");
                return;
            }
        }

        BackupEngine engine(*ctx.vault, ctx.tg_client);
        print_info(std::format("Restoring snapshot {} to {}...", snapshot_id, output_dir));
        ProgressBar bar;
        bool ok = engine.restore_snapshot(snapshot_id, output_dir, pass,
            [&bar](const ProgressInfo& p) {
                bar.update(p.current, p.total, p.stage);
            });
        bar.finish();
        if (ok) {
            print_success("Snapshot restored successfully to: " + output_dir);
        } else {
            print_error("Failed to restore snapshot: " + snapshot_id);
        }
    }

    void cmd_backup_prune(AppContext& ctx, int daily, int weekly, int monthly) {
        ensure_vault(ctx);
        BackupEngine engine(*ctx.vault, ctx.tg_client);
        RetentionPolicy pol;
        pol.daily = daily;
        pol.weekly = weekly;
        pol.monthly = monthly;
        if (engine.prune_snapshots(pol)) {
            print_success("Snapshots pruned according to retention policy.");
        } else {
            print_error("Failed to prune snapshots");
        }
    }

    void cmd_backup_delete(AppContext& ctx, const std::string& snapshot_id) {
        ensure_vault(ctx);
        BackupEngine engine(*ctx.vault, ctx.tg_client);
        if (engine.delete_snapshot(snapshot_id)) {
            print_success("Snapshot deleted: " + snapshot_id);
        } else {
            print_error("Failed to delete snapshot: " + snapshot_id);
        }
    }

    // ── Watch ─────────────────────────────────────────────────────────
    static std::atomic<bool> g_stop_watching{false};

    void watch_sig_handler(int sig) {
        if (sig == SIGINT || sig == SIGTERM) {
            g_stop_watching.store(true, std::memory_order_relaxed);
        }
    }

    // Blocks until Ctrl+C, then stops the given server. `tvt serve` /
    // `tvt serve-s3` previously returned right after non-blocking start(),
    // so the process exited and the servers never actually served.
    template <typename Server>
    void serve_until_interrupted(Server& server, std::string_view label) {
        g_stop_watching.store(false);
        auto prev_handler = std::signal(SIGINT, watch_sig_handler);
        auto prev_term = std::signal(SIGTERM, watch_sig_handler);
        std::println("Press Ctrl+C to stop the {} server.", label);
        while (!g_stop_watching.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        print_info(std::format("Stopping {} server...", label));
        server.stop();
        std::signal(SIGINT, prev_handler);
        std::signal(SIGTERM, prev_term);
        print_success(std::format("{} server stopped cleanly.", label));
    }

    void cmd_watch(AppContext& ctx, const std::string& dir, const std::string& password,
                   const std::vector<std::string>& exclusions) {
        ensure_vault(ctx);
        if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
            print_error("Directory does not exist or is not a directory: " + dir);
            return;
        }

        auto& cfg = ConfigManager::instance().get();
        VaultOptions opts;
        opts.encrypted = cfg.encryption;
        if (opts.encrypted) {
            opts.password = resolve_password(password);
            if (opts.password.empty()) {
                print_error("Password cannot be empty when encryption is enabled");
                return;
            }
        }
        opts.compressed = cfg.compression;

        g_stop_watching.store(false);
        auto prev_handler = std::signal(SIGINT, watch_sig_handler);

        std::println("\033[1;36m[televault] Watching directory: {}\033[0m", std::filesystem::absolute(dir).string());
        std::println("Press Ctrl+C to stop watching.");

        FileWatcher watcher(dir);
        if (!exclusions.empty()) {
            watcher.set_exclusions(exclusions);
        }

        std::mutex push_mutex;
        watcher.start([&ctx, &opts, &push_mutex](const std::vector<std::string>& changed) {
            std::lock_guard lock(push_mutex);
            for (const auto& path : changed) {
                if (path.starts_with("[DELETED] ")) {
                    std::println("\033[33m~ File deleted locally:\033[0m {}", path.substr(10));
                    continue;
                }
                std::println("\033[36m⚡ Change detected:\033[0m {}", path);
                ProgressBar bar;
                bool ok = ctx.vault->push(path, opts, [&bar](const ProgressInfo& p) {
                    bar.update(p.current, p.total, p.stage);
                });
                bar.finish();
                if (ok) {
                    std::println("\033[32m✓ Synced to vault:\033[0m {}", path);
                } else {
                    std::println("\033[31m✗ Sync failed:\033[0m {}", path);
                }
            }
        });

        while (!g_stop_watching.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        print_info("Stopping watcher...");
        watcher.stop();
        std::signal(SIGINT, prev_handler);
        print_success("Watcher stopped cleanly.");
    }

    // ── Advanced commands ─────────────────────────────────────────────
    void cmd_mount(AppContext& ctx, const std::string& mountpoint, const std::string& password,
                   uint64_t cache_mb, bool read_only) {
#if defined(TV_BUILD_FUSE)
        ensure_vault(ctx);
        FuseOptions opts;
        opts.mount_point = mountpoint;
        opts.read_only = read_only;
        opts.cache_size_mb = cache_mb;
        opts.password = resolve_password(password);

        print_info(std::format("Mounting TeleVault on '{}' (cache: {} MB, read-only: {})...",
                               mountpoint, cache_mb, read_only ? "yes" : "no"));
        TeleVaultFuse fuse(*ctx.vault);
        if (!fuse.mount(opts)) {
            print_error("Failed to mount FUSE filesystem.");
        }
#else
        print_error("TeleVault was built without FUSE support.");
#endif
    }

    void cmd_serve(AppContext& ctx, const std::string& host, uint16_t port,
                   const std::string& password, bool read_only) {
#if defined(TV_BUILD_WEBDAV)
        ensure_vault(ctx);
        WebDAVOptions opts;
        opts.host = host;
        opts.port = port;
        opts.read_only = read_only;
        opts.password = resolve_password(password);

        print_info(std::format("Starting WebDAV server on http://{}:{} (read-only: {})...",
                               host, port, read_only ? "yes" : "no"));
        WebDAVServer server(*ctx.vault);
        if (!server.start(opts)) {
            print_error("Failed to start WebDAV server.");
            return;
        }
        serve_until_interrupted(server, "WebDAV");
#else
        print_error("TeleVault was built without WebDAV support.");
#endif
    }

    void cmd_serve_s3(AppContext& ctx, const std::string& host, uint16_t port,
                      const std::string& password) {
#if defined(TV_BUILD_WEBDAV)
        ensure_vault(ctx);
        S3Options opts;
        opts.host = host;
        opts.port = port;
        opts.password = resolve_password(password);

        print_info(std::format("Starting S3-compatible gateway on http://{}:{} ...", host, port));
        print_info("Access Key: televault | Secret Key: televaultadmin");
        S3Server server(*ctx.vault);
        if (!server.start(opts)) {
            print_error("Failed to start S3 server.");
            return;
        }
        serve_until_interrupted(server, "S3");
#else
        print_error("TeleVault was built without WebDAV/S3 support.");
#endif
    }

    void cmd_share(AppContext& ctx, const std::string& path, const std::string& host,
                   uint16_t port, const std::string& expires_str, const std::string& pin,
                   const std::string& password) {
#if defined(TV_BUILD_WEBDAV)
        ensure_vault(ctx);
        ShareOptions opts;
        opts.remote_path = path;
        opts.host = host;
        opts.port = port;
        opts.pin = pin;

        uint64_t seconds = 3600;
        if (!expires_str.empty()) {
            if (expires_str == "0" || expires_str == "never") {
                seconds = 0;
            } else if (expires_str.ends_with("h") || expires_str.ends_with("H")) {
                seconds = std::stoull(expires_str.substr(0, expires_str.size() - 1)) * 3600;
            } else if (expires_str.ends_with("m") || expires_str.ends_with("M")) {
                seconds = std::stoull(expires_str.substr(0, expires_str.size() - 1)) * 60;
            } else if (expires_str.ends_with("d") || expires_str.ends_with("D")) {
                seconds = std::stoull(expires_str.substr(0, expires_str.size() - 1)) * 86400;
            } else if (expires_str.ends_with("s") || expires_str.ends_with("S")) {
                seconds = std::stoull(expires_str.substr(0, expires_str.size() - 1));
            } else {
                seconds = std::stoull(expires_str);
            }
        }
        opts.expires_in = std::chrono::seconds(seconds);

        auto meta = ctx.vault->get_file_info(path);
        if (!meta) {
            print_error("File not found in vault: " + path);
            return;
        }
        // The -p flag was previously accepted but silently dropped, so
        // shares of encrypted files always failed decrypt with HTTP 500.
        opts.password = resolve_password(password);
        if (meta->encrypted && opts.password.empty()) {
            print_error("Password required to share encrypted file");
            return;
        }
        // Preflight so a wrong password fails here, not per-request.
        {
            VaultOptions vopts;
            vopts.password = opts.password;
            if (!ctx.vault->read_byte_range(meta->name, 0, 0, vopts)) {
                print_error("Cannot decrypt '" + meta->name +
                            "': wrong password or corrupted chunk.");
                return;
            }
        }

        ShareServer server(*ctx.vault);
        std::println("\033[36m╭──────────────────────────────────────────────────╮\033[0m");
        std::println("\033[36m│\033[0m  \033[1;32mTeleVault Ephemeral Direct Share Link\033[0m           \033[36m│\033[0m");
        std::println("\033[36m╰──────────────────────────────────────────────────╯\033[0m");
        std::println("  File:       \033[1m{}\033[0m", path);
        if (seconds > 0) {
            std::println("  Expires in: \033[33m{}\033[0m", expires_str);
        } else {
            std::println("  Expires:    \033[33mNever\033[0m");
        }
        if (!pin.empty()) {
            std::println("  PIN code:   \033[35m{}\033[0m", pin);
        }

        server.start(opts, true);
#else
        print_error("TeleVault was built without WebDAV/Share support.");
#endif
    }

    void cmd_trash_list(AppContext& ctx) {
        ensure_vault(ctx);
        auto trashed = ctx.vault->list_trash();
        if (trashed.empty()) {
            print_info("Encrypted trash is empty.");
            return;
        }

        std::println("{:<40} {:<12} {:<24} {:<6}", "ORIGINAL PATH", "SIZE", "TRASHED AT", "VERSION");
        std::println("{:-<40} {:-<12} {:-<24} {:-<6}", "", "", "", "");
        for (const auto& f : trashed) {
            std::println("{:<40} {:<12} {:<24} v{:<5}",
                         f.name, format_size(f.size), f.created_at, f.version);
        }
        print_info(std::format("Total trashed files: {}", trashed.size()));
    }

    void cmd_trash_empty(AppContext& ctx) {
        ensure_vault(ctx);
        size_t count = ctx.vault->empty_trash();
        print_success(std::format("Emptied trash: permanently removed {} files.", count));
    }

    void cmd_restore(AppContext& ctx, const std::string& path) {
        ensure_vault(ctx);
        if (ctx.vault->restore_file(path)) {
            print_success(std::format("Restored file from trash: {}", path));
        } else {
            print_error(std::format("Could not restore '{}' (file not found in trash).", path));
        }
    }

    void cmd_versions(AppContext& ctx, const std::string& path) {
        ensure_vault(ctx);
        auto fi = ctx.vault->get_file_info(path);
        if (!fi) {
            print_error("File not found in vault: " + path);
            return;
        }
        std::println("\033[1;36mVersions for '{}':\033[0m", path);
        std::println("  Version:      v{}", fi->version);
        std::println("  Current Size: {}", format_size(fi->size));
        std::println("  Created:      {}", fi->created_at);
        std::println("  Encrypted:    {}", fi->encrypted ? "Yes (AES-256-GCM)" : "No");
        std::println("  Chunks:       {}", fi->chunk_count());
    }

    void cmd_schedule_create(AppContext& ctx, const std::string& name, const std::string& path,
                             const std::string& interval, const std::string& password,
                             bool incremental, const std::vector<std::string>& excludes, bool install) {
        ensure_vault(ctx);
        Interval iv = Interval::Daily;
        if (!interval_from_string(interval, iv)) {
            print_error("Invalid interval '" + interval + "' (use hourly, daily, weekly, monthly)");
            return;
        }
        ScheduleEntry e;
        e.name = name;
        e.path = path;
        e.interval = iv;
        // Prompt when neither -p nor TELEVAULT_PASSWORD is set, so timer
        // runs have a stored password (kept in a 0600 .env companion file).
        e.password = password.empty() ? resolve_password("") : password;
        e.incremental = incremental;
        e.exclude_patterns = excludes;
        if (install && e.password.empty()) {
            print_error("A password (-p or TELEVAULT_PASSWORD) is required to install a timer run.");
            return;
        }

        ScheduleManager mgr;
        if (!mgr.create(e)) {
            print_error("Failed to create schedule '" + name + "'");
            return;
        }
        print_success("Schedule '" + name + "' created (" + interval_to_string(iv) + ").");
        if (install) {
            if (mgr.install_systemd_timer(e)) {
                print_success("Systemd timer installed and started.");
            } else {
                print_info("Cron fallback — add this line with 'crontab -e':");
                std::println("  {}", mgr.generate_cron_entry(e));
            }
        } else {
            print_info("Run 'tvt schedule install " + name + "' to enable automatic runs.");
        }
    }

    void cmd_schedule_list(AppContext&) {
        ScheduleManager mgr;
        auto entries = mgr.list();
        if (entries.empty()) {
            print_info("No schedules defined. Create one with 'tvt schedule create'.");
            return;
        }
        std::println("{:<20} {:<10} {:<30} {:<12}", "NAME", "INTERVAL", "PATH", "LAST RUN");
        std::println("{:-<20} {:-<10} {:-<30} {:-<12}", "", "", "", "");
        for (auto& e : entries) {
            std::string last = "never";
            if (e.last_run.time_since_epoch().count() > 0) {
                auto t = std::chrono::system_clock::to_time_t(e.last_run);
                std::tm tm_buf{};
                localtime_r(&t, &tm_buf);
                char buf[32];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
                last = buf;
            }
            std::println("{:<20} {:<10} {:<30} {:<12}", e.name, interval_to_string(e.interval), e.path, last);
        }
    }

    void cmd_schedule_run(AppContext& ctx, const std::string& name) {
        ensure_vault(ctx);
        ScheduleManager mgr;
        if (mgr.run(name, *ctx.vault, ctx.tg_client)) {
            print_success("Scheduled snapshot '" + name + "' completed.");
        } else {
            print_error("Scheduled run '" + name + "' failed.");
        }
    }

    void cmd_schedule_remove(AppContext&, const std::string& name) {
        ScheduleManager mgr;
        if (mgr.remove(name)) {
            print_success("Schedule '" + name + "' removed.");
        } else {
            print_error("Schedule '" + name + "' not found.");
        }
    }

    void cmd_schedule_install(AppContext&, const std::string& name) {
        ScheduleManager mgr;
        ScheduleEntry e;
        if (!mgr.load(name, e)) {
            print_error("Schedule '" + name + "' not found.");
            return;
        }
        if (mgr.install_systemd_timer(e)) {
            print_success("Systemd timer installed and started.");
        } else {
            print_info("Cron fallback — add this line with 'crontab -e':");
            std::println("  {}", mgr.generate_cron_entry(e));
        }
    }

    void cmd_schedule_uninstall(AppContext&, const std::string& name) {
        ScheduleManager mgr;
        if (mgr.uninstall_systemd_timer(name)) {
            print_success("Systemd timer removed.");
        } else {
            print_error("No timer found for schedule '" + name + "'.");
        }
    }

} // anonymous namespace

void build_cli(CLI::App& app, AppContext& ctx) {
    app.description(std::format("TeleVault v{} — Encrypt → Chunk → Upload to Telegram", TELEVAULT_VERSION));

    // ── Auth subcommands ──────────────────────────────────────────────
    auto* login = app.add_subcommand("login", "Authenticate with Telegram");
    auto login_qr = std::make_shared<bool>(false);
    login->add_flag("--qr", *login_qr, "Log in by scanning a QR code (no SMS code needed)");
    login->callback([&ctx, login_qr]() { cmd_login(ctx, *login_qr); });

    auto* logout = app.add_subcommand("logout", "Clear stored session");
    logout->callback([&ctx]() { cmd_logout(ctx); });

    auto* setup = app.add_subcommand("setup", "Configure storage channel");
    setup->callback([&ctx]() { cmd_setup(ctx); });

    auto* channel = app.add_subcommand("channel", "Show current channel info");
    channel->callback([&ctx]() { cmd_channel(ctx); });

    auto* whoami = app.add_subcommand("whoami", "Show Telegram account info");
    whoami->callback([&ctx]() { cmd_whoami(ctx); });

    // ── File operation subcommands ────────────────────────────────────
    struct PushArgs {
        std::string path;
        std::string password;
        bool recursive{};
        bool resume{};
        bool low{};
        bool no_encryption{};
        bool delta{};
        std::vector<int64_t> shards{};
    };
    auto push_args = std::make_shared<PushArgs>();
    auto* push = app.add_subcommand("push", "Upload a file");
    push->add_option("path", push_args->path, "File or directory to upload")->required();
    push->add_option("-p,--password", push_args->password, "Encryption password (or set TELEVAULT_PASSWORD)");
    push->add_flag("-r,--recursive", push_args->recursive, "Upload directory recursively");
    push->add_flag("--resume", push_args->resume, "Resume interrupted upload");
    push->add_flag("--low-resource", push_args->low, "Low-resource mode");
    push->add_flag("--no-encryption", push_args->no_encryption, "Disable encryption");
    push->add_flag("--delta", push_args->delta, "Client-side delta sync (reuse unchanged chunks)");
    push->add_option("--shards", push_args->shards, "Additional storage channel IDs for RAID-0 striping");
    push->callback([&ctx, push_args]() {
        cmd_push(ctx, push_args->path, push_args->password, push_args->recursive,
                 push_args->resume, push_args->low, push_args->no_encryption,
                 push_args->delta, push_args->shards);
    });

    struct PullArgs {
        std::string path;
        std::string output;
        std::string password;
        bool resume{};
        bool low{};
    };
    auto pull_args = std::make_shared<PullArgs>();
    auto* pull = app.add_subcommand("pull", "Download a file from vault (interactive if no file specified)");
    pull->add_option("path", pull_args->path, "File name, ID, or substring to download");
    pull->add_option("-o,--output", pull_args->output, "Output path (use '-' for stdout)");
    pull->add_option("-p,--password", pull_args->password, "Decryption password (or set TELEVAULT_PASSWORD)");
    pull->add_flag("--resume", pull_args->resume, "Resume interrupted download");
    pull->add_flag("--low-resource", pull_args->low, "Low-resource mode");
    pull->callback([&ctx, pull_args]() {
        cmd_pull(ctx, pull_args->path, pull_args->output, pull_args->password,
                 pull_args->resume, pull_args->low);
    });

    struct LsArgs {
        bool json{};
        std::string sort;
        bool wide{};
    };
    auto ls_args = std::make_shared<LsArgs>();
    auto* ls = app.add_subcommand("ls", "List files");
    ls->add_flag("--json", ls_args->json, "JSON output");
    ls->add_flag("-w,--wide", ls_args->wide, "Disable name truncation");
    ls->add_option("--sort", ls_args->sort, "Sort field (name, size)");
    ls->callback([&ctx, ls_args]() { cmd_ls(ctx, ls_args->json, ls_args->sort, ls_args->wide); });

    struct CatArgs {
        std::string path;
        std::string password;
    };
    auto cat_args = std::make_shared<CatArgs>();
    auto* cat = app.add_subcommand("cat", "Stream file to stdout");
    cat->add_option("path", cat_args->path, "File path in vault")->required();
    cat->add_option("-p,--password", cat_args->password, "Decryption password (or set TELEVAULT_PASSWORD)");
    cat->callback([&ctx, cat_args]() { cmd_cat(ctx, cat_args->path, cat_args->password); });

    struct FindArgs {
        std::string query;
        bool json{};
        std::string ext;
        int64_t min_size{-1};
        int64_t max_size{-1};
    };
    auto find_args = std::make_shared<FindArgs>();
    auto* find = app.add_subcommand("find", "Search files by name or pattern");
    find->alias("search");
    find->add_option("query", find_args->query, "Search query");
    find->add_flag("--json", find_args->json, "JSON output");
    find->add_option("-e,--ext", find_args->ext, "Filter by file extension (e.g. .mp4 or mp4)");
    find->add_option("--min-size", find_args->min_size, "Filter by minimum file size in bytes");
    find->add_option("--max-size", find_args->max_size, "Filter by maximum file size in bytes");
    find->callback([&ctx, find_args]() {
        cmd_find(ctx, find_args->query, find_args->json, find_args->ext, find_args->min_size, find_args->max_size);
    });

    // ── Streaming subcommand ──────────────────────────────────────────
    struct StreamArgs {
        std::string path;
        uint16_t port{8080};
        std::string password;
    };
    auto stream_args = std::make_shared<StreamArgs>();
    auto* stream = app.add_subcommand("stream", "Stream media file with HTTP Range support");
    stream->add_option("path", stream_args->path, "File path in vault to stream")->required();
    stream->add_option("--port", stream_args->port, "HTTP port (default: 8080)");
    stream->add_option("-p,--password", stream_args->password, "Decryption password (or set TELEVAULT_PASSWORD)");
    stream->callback([&ctx, stream_args]() {
        cmd_stream(ctx, stream_args->path, stream_args->port, stream_args->password);
    });

    // ── Completion subcommand ─────────────────────────────────────────
    struct CompletionArgs {
        std::string shell{"bash"};
    };
    auto comp_args = std::make_shared<CompletionArgs>();
    auto* completion = app.add_subcommand("completion", "Generate shell autocompletion script (bash, zsh, fish)");
    completion->add_option("shell", comp_args->shell, "Shell type: bash, zsh, fish")->check(CLI::IsMember({"bash", "zsh", "fish"}));
    completion->callback([comp_args]() {
        cmd_completion(comp_args->shell);
    });

    auto* complete_files = app.add_subcommand("__complete_files", "")->group("");
    complete_files->callback([&ctx]() {
        cmd_complete_files(ctx);
    });

    struct InfoArgs {
        std::string path;
        bool json{};
    };
    auto info_args = std::make_shared<InfoArgs>();
    auto* info = app.add_subcommand("info", "Detailed file info");
    info->add_option("path", info_args->path, "File path in vault")->required();
    info->add_flag("--json", info_args->json, "JSON output");
    info->callback([&ctx, info_args]() { cmd_info(ctx, info_args->path, info_args->json); });

    auto stat_json = std::make_shared<bool>(false);
    auto* stat = app.add_subcommand("stat", "Vault statistics");
    stat->add_flag("--json", *stat_json, "JSON output");
    stat->callback([&ctx, stat_json]() { cmd_stat(ctx, *stat_json); });

    struct RmArgs {
        std::string path;
        bool purge{};
    };
    auto rm_args = std::make_shared<RmArgs>();
    auto* rm = app.add_subcommand("rm", "Delete a file");
    rm->add_option("path", rm_args->path, "File path in vault")->required();
    rm->add_flag("--purge", rm_args->purge, "Permanently delete file instead of moving to trash");
    rm->callback([&ctx, rm_args]() { cmd_rm(ctx, rm_args->path, rm_args->purge); });

    auto verify_path = std::make_shared<std::string>();
    auto* verify_cmd = app.add_subcommand("verify", "Verify file integrity");
    verify_cmd->add_option("path", *verify_path, "File path in vault")->required();
    verify_cmd->callback([&ctx, verify_path]() { cmd_verify(ctx, *verify_path); });

    auto* recover = app.add_subcommand("recover", "Reconstruct vault index from channel history");
    recover->callback([&ctx]() { cmd_recover(ctx); });

    auto* sync_cmd = app.add_subcommand("sync", "Synchronize vault index across multiple machines");
    sync_cmd->callback([&ctx]() { cmd_sync(ctx); });

    // ── GC ────────────────────────────────────────────────────────────
    struct GcArgs {
        bool force{};
        bool clean{};
    };
    auto gc_args = std::make_shared<GcArgs>();
    auto* gc = app.add_subcommand("gc", "Garbage collection");
    gc->add_flag("--force", gc_args->force, "Actually delete orphans");
    gc->add_flag("--clean-partials", gc_args->clean, "Clean partial uploads");
    gc->callback([&ctx, gc_args]() { cmd_gc(ctx, gc_args->force, gc_args->clean); });

    // ── TUI ───────────────────────────────────────────────────────────
    auto* tui = app.add_subcommand("tui", "Launch terminal UI");
    tui->callback([&ctx]() { cmd_tui(ctx); });

    // ── Preview ───────────────────────────────────────────────────────
    struct PreviewArgs {
        std::string path;
        std::string password;
    };
    auto prev_args = std::make_shared<PreviewArgs>();
    auto* preview = app.add_subcommand("preview", "Preview a file");
    preview->add_option("path", prev_args->path, "File path in vault or local file")->required();
    preview->add_option("-p,--password", prev_args->password, "Decryption password (or set TELEVAULT_PASSWORD)");
    preview->callback([&ctx, prev_args]() { cmd_preview(ctx, prev_args->path, prev_args->password); });

    // ── Advanced subcommands ──────────────────────────────────────────
    struct MountArgs {
        std::string mountpoint;
        std::string password;
        uint64_t cache_mb{256};
        bool read_only{true};
    };
    auto mount_args = std::make_shared<MountArgs>();
    auto* mount = app.add_subcommand("mount", "Mount FUSE filesystem");
    mount->add_option("mountpoint", mount_args->mountpoint, "Local directory mount point")->required();
    mount->add_option("-p,--password", mount_args->password, "Decryption password");
    mount->add_option("--cache-size", mount_args->cache_mb, "Chunk cache size in MB (default: 256)");
    mount->add_flag("--read-only", mount_args->read_only, "Mount as read-only (default: true)");
    mount->callback([&ctx, mount_args]() {
        cmd_mount(ctx, mount_args->mountpoint, mount_args->password, mount_args->cache_mb, mount_args->read_only);
    });

    struct ServeArgs {
        std::string host{"127.0.0.1"};
        uint16_t port{8080};
        std::string password;
        bool read_only{true};
    };
    auto serve_args = std::make_shared<ServeArgs>();
    auto* serve = app.add_subcommand("serve", "Start WebDAV server");
    serve->add_option("-H,--host", serve_args->host, "Host address (default: 127.0.0.1)");
    serve->add_option("-p,--port", serve_args->port, "Port number (default: 8080)");
    serve->add_option("--password", serve_args->password, "Decryption password");
    serve->add_flag("--read-only", serve_args->read_only, "Read only mode (default: true)");
    serve->callback([&ctx, serve_args]() {
        cmd_serve(ctx, serve_args->host, serve_args->port, serve_args->password, serve_args->read_only);
    });

    struct S3Args {
        std::string host{"0.0.0.0"};
        uint16_t port{9000};
        std::string password;
    };
    auto s3_args = std::make_shared<S3Args>();
    auto* s3 = app.add_subcommand("serve-s3", "Start S3-compatible storage gateway");
    s3->add_option("-H,--host", s3_args->host, "Host address (default: 0.0.0.0)");
    s3->add_option("-p,--port", s3_args->port, "Port number (default: 9000)");
    s3->add_option("--password", s3_args->password, "Encryption/decryption password");
    s3->callback([&ctx, s3_args]() {
        cmd_serve_s3(ctx, s3_args->host, s3_args->port, s3_args->password);
    });

    struct ShareArgs {
        std::string path;
        std::string host{"0.0.0.0"};
        uint16_t port{8080};
        std::string expires{"1h"};
        std::string pin;
        std::string password;
    };
    auto share_args = std::make_shared<ShareArgs>();
    auto* share = app.add_subcommand("share", "Generate ephemeral direct share link");
    share->add_option("path", share_args->path, "File path in vault to share")->required();
    share->add_option("-H,--host", share_args->host, "Host address (default: 0.0.0.0)");
    share->add_option("--port", share_args->port, "Port number (default: 8080)");
    share->add_option("--expires", share_args->expires, "Link expiration time (e.g. 1h, 30m, 24h, 0 for never)");
    share->add_option("--pin", share_args->pin, "Optional PIN code protection");
    share->add_option("-p,--password", share_args->password, "Decryption password");
    share->callback([&ctx, share_args]() {
        cmd_share(ctx, share_args->path, share_args->host, share_args->port, share_args->expires, share_args->pin, share_args->password);
    });

    auto* trash = app.add_subcommand("trash", "Encrypted trash bin management");
    auto* trash_list = trash->add_subcommand("list", "List files in trash");
    trash_list->callback([&ctx]() { cmd_trash_list(ctx); });
    auto* trash_empty = trash->add_subcommand("empty", "Permanently remove all files in trash");
    trash_empty->callback([&ctx]() { cmd_trash_empty(ctx); });
    trash->callback([&ctx]() { cmd_trash_list(ctx); });

    auto restore_path = std::make_shared<std::string>();
    auto* restore = app.add_subcommand("restore", "Restore file from encrypted trash");
    restore->add_option("path", *restore_path, "File path to restore")->required();
    restore->callback([&ctx, restore_path]() { cmd_restore(ctx, *restore_path); });

    auto versions_path = std::make_shared<std::string>();
    auto* versions = app.add_subcommand("versions", "List file version history");
    versions->add_option("path", *versions_path, "File path in vault")->required();
    versions->callback([&ctx, versions_path]() { cmd_versions(ctx, *versions_path); });

    auto* backup = app.add_subcommand("backup", "Snapshot backup management");
    backup->require_subcommand(1);

    struct BackupCreateArgs {
        std::vector<std::string> paths;
        std::string name;
        std::string password;
        bool incremental{};
    };
    auto bc_args = std::make_shared<BackupCreateArgs>();
    auto* bk_create = backup->add_subcommand("create", "Create a new snapshot");
    bk_create->add_option("paths", bc_args->paths, "Paths to include in snapshot")->required();
    bk_create->add_option("-n,--name", bc_args->name, "Snapshot name");
    bk_create->add_option("-p,--password", bc_args->password, "Encryption password (or set TELEVAULT_PASSWORD)");
    bk_create->add_flag("--incremental", bc_args->incremental, "Incremental snapshot");
    bk_create->callback([&ctx, bc_args]() {
        cmd_backup_create(ctx, bc_args->paths, bc_args->name, bc_args->password, bc_args->incremental);
    });

    auto* bk_list = backup->add_subcommand("list", "List snapshots");
    bk_list->callback([&ctx]() { cmd_backup_list(ctx); });

    struct BackupRestoreArgs {
        std::string id;
        std::string output = ".";
        std::string password;
    };
    auto br_args = std::make_shared<BackupRestoreArgs>();
    auto* bk_restore = backup->add_subcommand("restore", "Restore a snapshot");
    bk_restore->add_option("id", br_args->id, "Snapshot ID")->required();
    bk_restore->add_option("-o,--output", br_args->output, "Output directory (default: current directory)");
    bk_restore->add_option("-p,--password", br_args->password, "Decryption password (or set TELEVAULT_PASSWORD)");
    bk_restore->callback([&ctx, br_args]() {
        cmd_backup_restore(ctx, br_args->id, br_args->output, br_args->password);
    });

    struct BackupPruneArgs {
        int daily{7};
        int weekly{4};
        int monthly{6};
    };
    auto bp_args = std::make_shared<BackupPruneArgs>();
    auto* bk_prune = backup->add_subcommand("prune", "Prune old snapshots according to retention policy");
    bk_prune->add_option("--keep-daily", bp_args->daily, "Keep daily snapshots (default: 7)");
    bk_prune->add_option("--keep-weekly", bp_args->weekly, "Keep weekly snapshots (default: 4)");
    bk_prune->add_option("--keep-monthly", bp_args->monthly, "Keep monthly snapshots (default: 6)");
    bk_prune->callback([&ctx, bp_args]() {
        cmd_backup_prune(ctx, bp_args->daily, bp_args->weekly, bp_args->monthly);
    });

    struct BackupDeleteArgs {
        std::string id;
    };
    auto bd_args = std::make_shared<BackupDeleteArgs>();
    auto* bk_del = backup->add_subcommand("delete", "Delete a snapshot");
    bk_del->alias("rm");
    bk_del->add_option("id", bd_args->id, "Snapshot ID")->required();
    bk_del->callback([&ctx, bd_args]() { cmd_backup_delete(ctx, bd_args->id); });

    auto* schedule = app.add_subcommand("schedule", "Backup scheduling");
    schedule->require_subcommand(1);

    struct ScheduleCreateArgs {
        std::string name;
        std::string path;
        std::string interval{"daily"};
        std::string password;
        bool incremental{true};
        bool full{};
        std::vector<std::string> excludes;
        bool no_install{};
    };
    auto sc_args = std::make_shared<ScheduleCreateArgs>();
    auto* sc_create = schedule->add_subcommand("create", "Create a backup schedule");
    sc_create->add_option("-n,--name", sc_args->name, "Schedule name")->required();
    sc_create->add_option("--path", sc_args->path, "Directory to back up")->required();
    sc_create->add_option("--interval", sc_args->interval, "hourly, daily, weekly, monthly");
    sc_create->add_option("-p,--password", sc_args->password, "Encryption password (or set TELEVAULT_PASSWORD)");
    sc_create->add_flag("--incremental", sc_args->incremental, "Incremental snapshots (default)");
    sc_create->add_flag("--full", sc_args->full, "Full snapshots instead of incremental");
    sc_create->add_option("--exclude", sc_args->excludes, "Exclude pattern (repeatable)");
    sc_create->add_flag("--no-install", sc_args->no_install, "Only save the schedule, do not install the timer");
    sc_create->callback([&ctx, sc_args]() {
        cmd_schedule_create(ctx, sc_args->name, sc_args->path, sc_args->interval,
                            sc_args->password, !sc_args->full, sc_args->excludes,
                            !sc_args->no_install);
    });

    auto* sc_list = schedule->add_subcommand("list", "List schedules");
    sc_list->callback([&ctx]() { cmd_schedule_list(ctx); });

    auto sc_name = std::make_shared<std::string>();
    auto* sc_run = schedule->add_subcommand("run", "Run a schedule now");
    sc_run->add_option("name", *sc_name, "Schedule name")->required();
    sc_run->callback([&ctx, sc_name]() { cmd_schedule_run(ctx, *sc_name); });

    auto sc_rm = std::make_shared<std::string>();
    auto* sc_remove = schedule->add_subcommand("remove", "Delete a schedule");
    sc_remove->add_option("name", *sc_rm, "Schedule name")->required();
    sc_remove->callback([&ctx, sc_rm]() { cmd_schedule_remove(ctx, *sc_rm); });

    auto sc_in = std::make_shared<std::string>();
    auto* sc_install = schedule->add_subcommand("install", "Install systemd timer (or show cron line)");
    sc_install->add_option("name", *sc_in, "Schedule name")->required();
    sc_install->callback([&ctx, sc_in]() { cmd_schedule_install(ctx, *sc_in); });

    auto sc_un = std::make_shared<std::string>();
    auto* sc_uninstall = schedule->add_subcommand("uninstall", "Remove systemd timer");
    sc_uninstall->add_option("name", *sc_un, "Schedule name")->required();
    sc_uninstall->callback([&ctx, sc_un]() { cmd_schedule_uninstall(ctx, *sc_un); });

    struct WatchArgs {
        std::string dir;
        std::string password;
        std::vector<std::string> exclusions;
    };
    auto watch_args = std::make_shared<WatchArgs>();
    auto* watch = app.add_subcommand("watch", "Watch directory for changes");
    watch->add_option("dir", watch_args->dir, "Directory to watch")->required();
    watch->add_option("-p,--password", watch_args->password, "Encryption password (or set TELEVAULT_PASSWORD)");
    watch->add_option("--exclude", watch_args->exclusions, "Patterns to exclude from watching");
    watch->callback([&ctx, watch_args]() {
        cmd_watch(ctx, watch_args->dir, watch_args->password, watch_args->exclusions);
    });
}

} // namespace tv
