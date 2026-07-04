#include "cli.hpp"
#include "progress.hpp"
#include "../core/app_context.hpp"
#include "../telegram/client.hpp"
#include "../telegram/auth.hpp"
#include "../telegram/session.hpp"
#include "../core/vault.hpp"
#include "../util/config.hpp"
#include "../util/format.hpp"

#include <iostream>
#include <print>
#include <format>
#include <string>
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

        if (sm.has_api_credentials() && ctx.tg_client.is_authorized()) {
            print_info("Already logged in. Use 'logout' first to re-authenticate.");
            return;
        }

        if (sm.has_api_credentials()) {
            // Already have credentials from config — push to client
            ctx.tg_client.set_api_params(sm.api_id(), sm.api_hash());
        } else {
            std::print("Enter API ID (from my.telegram.org): ");
            std::string api_id_str;
            std::getline(std::cin, api_id_str);

            std::print("Enter API Hash: ");
            std::string api_hash;
            std::getline(std::cin, api_hash);

            int32_t api_id = std::stoi(api_id_str);
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

    // ── File operations ──────────────────────────────────────────────
    void ensure_vault(AppContext& ctx) {
        if (!ctx.ensure_vault()) {
            throw std::runtime_error("Vault not initialized. Run 'setup' first.");
        }
    }

    void cmd_push(AppContext& ctx, const std::string& path, bool recursive, bool resume, bool low_resource) {
        ensure_vault(ctx);
        VaultOptions opts;
        opts.low_resource = low_resource;
        opts.resume = resume;

        bool ok = ctx.vault->push(path, opts);
        if (ok) print_success(std::format("Uploaded: {}", path));
        else print_error(std::format("Upload failed: {}", path));
    }

    void cmd_pull(AppContext& ctx, const std::string& path, const std::string& output, bool resume, bool low_resource) {
        ensure_vault(ctx);
        VaultOptions opts;
        opts.low_resource = low_resource;
        opts.resume = resume;

        auto output_path = output.empty() ? path : output;
        bool ok = ctx.vault->pull(path, output_path, opts);
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

    void cmd_cat(AppContext& ctx, const std::string& path) {
        ensure_vault(ctx);
        VaultOptions opts;
        auto& cfg = ConfigManager::instance().get();
        // Password not stored in config — user must provide via env or we prompt
        if (cfg.encryption) {
            std::print("Enter encryption password: ");
            std::getline(std::cin, opts.password);
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
    void cmd_preview(AppContext& ctx, const std::string& path) {
        ctx.initialize();
        print_info("Preview: " + path);
        // Preview implementation goes here
    }

    // ── Advanced commands (stubs) ─────────────────────────────────────
    void cmd_mount(AppContext& ctx) {
        print_info("FUSE mount — not yet implemented");
    }

    void cmd_serve(AppContext& ctx) {
        print_info("WebDAV server — not yet implemented");
    }

    void cmd_backup(AppContext& ctx, CLI::App* app) {
        print_info("Backup commands — not yet fully implemented");
    }

    void cmd_schedule(AppContext& ctx) {
        print_info("Schedule commands — not yet fully implemented");
    }

    void cmd_watch(AppContext& ctx) {
        print_info("Watch command — not yet fully implemented");
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
    auto* push = app.add_subcommand("push", "Upload a file");
    std::string push_path;
    bool push_recursive{}, push_resume{}, push_low{};
    push->add_option("path", push_path, "File or directory to upload")->required();
    push->add_flag("-r,--recursive", push_recursive, "Upload directory recursively");
    push->add_flag("--resume", push_resume, "Resume interrupted upload");
    push->add_flag("--low-resource", push_low, "Low-resource mode");
    push->callback([&ctx, &push_path, &push_recursive, &push_resume, &push_low]() {
        cmd_push(ctx, push_path, push_recursive, push_resume, push_low);
    });

    auto* pull = app.add_subcommand("pull", "Download a file");
    std::string pull_path, pull_output;
    bool pull_resume{}, pull_low{};
    pull->add_option("path", pull_path, "File path in vault")->required();
    pull->add_option("-o,--output", pull_output, "Output path (use '-' for stdout)");
    pull->add_flag("--resume", pull_resume, "Resume interrupted download");
    pull->add_flag("--low-resource", pull_low, "Low-resource mode");
    pull->callback([&ctx, &pull_path, &pull_output, &pull_resume, &pull_low]() {
        cmd_pull(ctx, pull_path, pull_output, pull_resume, pull_low);
    });

    auto* ls = app.add_subcommand("ls", "List files");
    bool ls_json{};
    std::string ls_sort;
    ls->add_flag("--json", ls_json, "JSON output");
    ls->add_option("--sort", ls_sort, "Sort field");
    ls->callback([&ctx, &ls_json, &ls_sort]() { cmd_ls(ctx, ls_json, ls_sort); });

    auto* cat = app.add_subcommand("cat", "Stream file to stdout");
    std::string cat_path;
    cat->add_option("path", cat_path, "File path in vault")->required();
    cat->callback([&ctx, &cat_path]() { cmd_cat(ctx, cat_path); });

    auto* find = app.add_subcommand("find", "Search files by name");
    std::string find_query;
    bool find_json{};
    find->add_option("query", find_query, "Search query")->required();
    find->add_flag("--json", find_json, "JSON output");
    find->callback([&ctx, &find_query, &find_json]() { cmd_find(ctx, find_query, find_json); });

    auto* info = app.add_subcommand("info", "Detailed file info");
    std::string info_path;
    bool info_json{};
    info->add_option("path", info_path, "File path in vault")->required();
    info->add_flag("--json", info_json, "JSON output");
    info->callback([&ctx, &info_path, &info_json]() { cmd_info(ctx, info_path, info_json); });

    auto* stat = app.add_subcommand("stat", "Vault statistics");
    bool stat_json{};
    stat->add_flag("--json", stat_json, "JSON output");
    stat->callback([&ctx, &stat_json]() { cmd_stat(ctx, stat_json); });

    auto* rm = app.add_subcommand("rm", "Delete a file");
    std::string rm_path;
    rm->add_option("path", rm_path, "File path in vault")->required();
    rm->callback([&ctx, &rm_path]() { cmd_rm(ctx, rm_path); });

    auto* verify_cmd = app.add_subcommand("verify", "Verify file integrity");
    std::string verify_path;
    verify_cmd->add_option("path", verify_path, "File path in vault")->required();
    verify_cmd->callback([&ctx, &verify_path]() { cmd_verify(ctx, verify_path); });

    // ── GC ────────────────────────────────────────────────────────────
    auto* gc = app.add_subcommand("gc", "Garbage collection");
    bool gc_force{}, gc_clean{};
    gc->add_flag("--force", gc_force, "Actually delete orphans");
    gc->add_flag("--clean-partials", gc_clean, "Clean partial uploads");
    gc->callback([&ctx, &gc_force, &gc_clean]() { cmd_gc(ctx, gc_force, gc_clean); });

    // ── TUI ───────────────────────────────────────────────────────────
    auto* tui = app.add_subcommand("tui", "Launch terminal UI");
    tui->callback([&ctx]() { cmd_tui(ctx); });

    // ── Preview ───────────────────────────────────────────────────────
    auto* preview = app.add_subcommand("preview", "Preview a file");
    std::string preview_path;
    preview->add_option("path", preview_path, "File path in vault")->required();
    preview->callback([&ctx, &preview_path]() { cmd_preview(ctx, preview_path); });

    // ── Advanced subcommands ──────────────────────────────────────────
    auto* mount = app.add_subcommand("mount", "Mount FUSE filesystem");
    mount->callback([&ctx]() { cmd_mount(ctx); });

    auto* serve = app.add_subcommand("serve", "Start WebDAV server");
    serve->callback([&ctx]() { cmd_serve(ctx); });

    auto* backup = app.add_subcommand("backup", "Snapshot backup management");
    backup->callback([&ctx, backup]() { cmd_backup(ctx, backup); });

    auto* schedule = app.add_subcommand("schedule", "Backup scheduling");
    schedule->callback([&ctx]() { cmd_schedule(ctx); });

    auto* watch = app.add_subcommand("watch", "Watch directory for changes");
    watch->callback([&ctx]() { cmd_watch(ctx); });
}

} // namespace tv
