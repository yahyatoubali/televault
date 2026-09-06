#include <iostream>
#include <cstdlib>
#include <csignal>
#include <atomic>
#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include "util/logging.hpp"
#include "util/config.hpp"
#include "cli/cli.hpp"
#include "core/app_context.hpp"

using namespace tv;

namespace {
    std::atomic<bool> g_shutdown{false};
    AppContext* g_app_ctx{};

    void signal_handler(int sig) {
        if (sig == SIGINT || sig == SIGTERM) {
            g_shutdown.store(true, std::memory_order_relaxed);
        }
    }
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    auto& cfg = ConfigManager::instance();
    cfg.load();

    Logging::setup(cfg.data_dir(), "info");

    AppContext app_ctx;
    g_app_ctx = &app_ctx;
    app_ctx.initialize();

    spdlog::info("TeleVault v{} starting", TELEVAULT_VERSION);

    CLI::App app{"TeleVault — Encrypted cloud storage on Telegram channels"};
    app.set_version_flag("-V,--version", std::string("TeleVault v") + TELEVAULT_VERSION);
    app.require_subcommand(0, 1);

    // Global options
    bool verbose{}, debug{};
    app.add_flag("-v,--verbose", verbose, "Verbose output");
    app.add_flag("--debug", debug, "Debug logging");

    // ── Build CLI ─────────────────────────────────────────────────────
    build_cli(app, app_ctx);

    // ── Parse ─────────────────────────────────────────────────────────
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        app_ctx.shutdown();
        return app.exit(e);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        app_ctx.shutdown();
        return 1;
    }

    if (debug) Logging::set_level("debug");

    app_ctx.shutdown();
    return 0;
}
