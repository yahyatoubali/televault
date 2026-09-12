#include "app_context.hpp"
#include "index.hpp"
#include "../telegram/client.hpp"
#include "../util/config.hpp"
#include <spdlog/spdlog.h>
#include <format>

namespace tv {

bool AppContext::initialize() {
    if (initialized) return true;

    auto& cfg = ConfigManager::instance();
    auto config = cfg.get();

    // Setup Telegram client with API credentials
    if (config.telegram.api_id > 0) {
        tg_client.set_api_params(config.telegram.api_id, config.telegram.api_hash);
    }

    initialized = true;
    return true;
}

bool AppContext::ensure_vault() {
    if (vault) return true;

    auto& cfg = ConfigManager::instance();
    auto config = cfg.get();
    if (config.channel_id == 0) return false;

    if (!tg_client.connect() || !tg_client.is_authorized()) {
        spdlog::warn("Telegram client not connected or authorized");
        return false;
    }

    vault = std::make_unique<TeleVault>(tg_client);
    if (!vault->initialize(config.channel_id, config.low_resource.enabled)) {
        vault.reset();
        return false;
    }
    // A configured channel that Telegram reports as inaccessible (logged in
    // with a different account than the channel owner, or a deleted
    // channel) must fail loudly here. Otherwise every command silently
    // reports an empty vault ("Total: 0 files") and users fear data loss.
    // Only applied when the index is empty: a reachable but empty vault is
    // a valid state for new users.
    if (vault->index_entries().empty() && !tg_client.is_valid_channel(config.channel_id)) {
        std::string who = tg_client.get_my_username();
        spdlog::error(
            "Storage channel {} is not accessible with this Telegram account{}. "
            "Your files are still on Telegram — this machine is simply looking "
            "in the wrong place. Run 'tvt whoami' to check the account and "
            "'tvt setup' to select the channel owned by this account.",
            config.channel_id, who.empty() ? "" : " (@" + who + ")");
        vault.reset();
        return false;
    }
    return true;
}

void AppContext::shutdown() {
    if (!initialized) return;
    vault.reset();
    initialized = false;
}

} // namespace tv
