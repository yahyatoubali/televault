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
#include <spdlog/spdlog.h>

namespace tv {

namespace {

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
    void cmd_login(AppContext& ctx) {
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
        std::getline(std::cin, phone);

        if (phone.empty()) {
            print_error("Phone number cannot be empty");
            return;
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
                  bool recursive, bool resume, bool low_resource, bool no_encryption) {
        ensure_vault(ctx);
        VaultOptions opts;
        opts.low_resource = low_resource;
        opts.resume = resume;

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

    void cmd_pull(AppContext& ctx, const std::string& path, const std::string& output,
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

        ProgressBar pb;
        auto cb = [&pb](const ProgressInfo& p) {
            if (!p.stage.empty()) {
                pb.set_message(p.stage);
            }
            if (p.total > 0) {
                pb.update(p.current, p.total);
            }
        };

        auto output_path = output.empty() ? path : output;
        bool ok = ctx.vault->pull(path, output_path, opts, cb);
        pb.finish();
        if (ok) print_success(std::format("Downloaded: {} → {}", path, output_path));
        else print_error(std::format("Download failed: {}", path));
    }

    void cmd_ls(AppContext& ctx, bool json_output, const std::string& sort) {
        ensure_vault(ctx);
        auto files = ctx.vault->list_files();

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

        std::println("{:<20} {:<40} {:>12}", "ID", "Name", "Size");
        std::println("{:-<20} {:-<40} {:->12}", "", "", "");
        for (auto& f : files) {
            auto short_id = f.id.substr(0, 16);
            auto disp_name = f.name.size() > 38 ? f.name.substr(0, 35) + "..." : f.name;
            std::println("{:<20} {:<40} {:>12}",
                         short_id, disp_name, format_size(f.size));
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

    void cmd_find(AppContext& ctx, const std::string& query, bool json_output) {
        ensure_vault(ctx);
        auto results = ctx.vault->find_files(query);
        for (auto& f : results) {
            if (json_output) {
                std::println("{{\"name\":\"{}\",\"size\":{}}}", f.name, f.size);
            } else {
                std::println("{} ({})", f.name, format_size(f.size));
            }
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

    void cmd_rm(AppContext& ctx, const std::string& path) {
        ensure_vault(ctx);
        if (ctx.vault->delete_file(path)) {
            print_success("Deleted: " + path);
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

    // ── GC ────────────────────────────────────────────────────────────
    void cmd_gc(AppContext& ctx, bool force, bool clean_partials) {
        ctx.initialize();
        print_info("GC: dry-run=" + std::string(!force ? "true" : "false") +
                   " clean_partials=" + std::string(clean_partials ? "true" : "false"));
        // GC implementation goes here
    }

    // ── TUI ───────────────────────────────────────────────────────────
    void cmd_tui(AppContext& ctx) {
        print_info("TUI mode — launching...");
        // FTXUI TUI implementation
        print_error("TUI not yet implemented");
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

    // ── Advanced commands (stubs) ─────────────────────────────────────
    void cmd_mount(AppContext& ctx) {
        print_info("FUSE mount — not yet implemented");
    }

    void cmd_serve(AppContext& ctx) {
        print_info("WebDAV server — not yet implemented");
    }

    void cmd_schedule(AppContext& ctx) {
        print_info("Schedule commands — not yet fully implemented");
    }

} // anonymous namespace

void build_cli(CLI::App& app, AppContext& ctx) {
    app.description(std::format("TeleVault v{} — Encrypt → Chunk → Upload to Telegram", TELEVAULT_VERSION));

    // ── Auth subcommands ──────────────────────────────────────────────
    auto* login = app.add_subcommand("login", "Authenticate with Telegram");
    login->callback([&ctx]() { cmd_login(ctx); });

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
    };
    auto push_args = std::make_shared<PushArgs>();
    auto* push = app.add_subcommand("push", "Upload a file");
    push->add_option("path", push_args->path, "File or directory to upload")->required();
    push->add_option("-p,--password", push_args->password, "Encryption password (or set TELEVAULT_PASSWORD)");
    push->add_flag("-r,--recursive", push_args->recursive, "Upload directory recursively");
    push->add_flag("--resume", push_args->resume, "Resume interrupted upload");
    push->add_flag("--low-resource", push_args->low, "Low-resource mode");
    push->add_flag("--no-encryption", push_args->no_encryption, "Disable encryption");
    push->callback([&ctx, push_args]() {
        cmd_push(ctx, push_args->path, push_args->password, push_args->recursive,
                 push_args->resume, push_args->low, push_args->no_encryption);
    });

    struct PullArgs {
        std::string path;
        std::string output;
        std::string password;
        bool resume{};
        bool low{};
    };
    auto pull_args = std::make_shared<PullArgs>();
    auto* pull = app.add_subcommand("pull", "Download a file");
    pull->add_option("path", pull_args->path, "File path in vault")->required();
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
    };
    auto ls_args = std::make_shared<LsArgs>();
    auto* ls = app.add_subcommand("ls", "List files");
    ls->add_flag("--json", ls_args->json, "JSON output");
    ls->add_option("--sort", ls_args->sort, "Sort field");
    ls->callback([&ctx, ls_args]() { cmd_ls(ctx, ls_args->json, ls_args->sort); });

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
    };
    auto find_args = std::make_shared<FindArgs>();
    auto* find = app.add_subcommand("find", "Search files by name");
    find->add_option("query", find_args->query, "Search query")->required();
    find->add_flag("--json", find_args->json, "JSON output");
    find->callback([&ctx, find_args]() { cmd_find(ctx, find_args->query, find_args->json); });

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

    auto rm_path = std::make_shared<std::string>();
    auto* rm = app.add_subcommand("rm", "Delete a file");
    rm->add_option("path", *rm_path, "File path in vault")->required();
    rm->callback([&ctx, rm_path]() { cmd_rm(ctx, *rm_path); });

    auto verify_path = std::make_shared<std::string>();
    auto* verify_cmd = app.add_subcommand("verify", "Verify file integrity");
    verify_cmd->add_option("path", *verify_path, "File path in vault")->required();
    verify_cmd->callback([&ctx, verify_path]() { cmd_verify(ctx, *verify_path); });

    auto* recover = app.add_subcommand("recover", "Reconstruct vault index from channel history");
    recover->callback([&ctx]() { cmd_recover(ctx); });

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
    auto* mount = app.add_subcommand("mount", "Mount FUSE filesystem");
    mount->callback([&ctx]() { cmd_mount(ctx); });

    auto* serve = app.add_subcommand("serve", "Start WebDAV server");
    serve->callback([&ctx]() { cmd_serve(ctx); });

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
    schedule->callback([&ctx]() { cmd_schedule(ctx); });

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
