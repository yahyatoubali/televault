#pragma once

#include <memory>
#include "../telegram/client.hpp"
#include "vault.hpp"

namespace tv {

struct AppContext {
    TelegramClient tg_client;
    std::unique_ptr<TeleVault> vault;
    bool initialized{};

    bool initialize();
    void shutdown();
};

} // namespace tv
