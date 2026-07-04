#include "app_context.hpp"
#include "index.hpp"
#include "../telegram/client.hpp"
#include "../util/config.hpp"
#include <spdlog/spdlog.h>

namespace tv {

bool AppContext::initialize() {
    if (initialized) return true;

    auto& cfg = ConfigManager::instance();
    auto config = cfg.get();

    // Setup Telegram client with API credentials
    if (config.telegram.api_id > 0) {
        tg_client.set_api_params(config.telegram.api_id, config.telegram.api_hash);
    }

    // Initialize vault with channel if configured
    if (config.channel_id != 0) {
        vault = std::make_unique<TeleVault>(tg_client);
        if (!vault->initialize(config.channel_id, config.low_resource.enabled)) {
            spdlog::warn("Vault initialization failed — run 'setup' first");
        }
    }

    initialized = true;
    return true;
}

void AppContext::shutdown() {
    if (!initialized) return;
    vault.reset();
    initialized = false;
}

} // namespace tv
