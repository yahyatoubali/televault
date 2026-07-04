#include "auth.hpp"
#include "client.hpp"
#include "../util/config.hpp"
#include <spdlog/spdlog.h>

namespace tv {

AuthFlow::AuthFlow(TelegramClient& client) : client_(client) {}

AuthFlow::State AuthFlow::execute(const std::string& phone, CodeCallback code_cb, PasswordCallback pw_cb) {
    state_ = State::Connecting;

    // Save phone to config
    auto& cfg = ConfigManager::instance();
    auto config = cfg.get();
    config.telegram.phone = phone;
    cfg.set(config);
    cfg.save();

    // Connect and authenticate via tdlib
    if (!client_.connect()) {
        spdlog::error("Failed to connect to Telegram");
        state_ = State::Failed;
        return state_;
    }

    if (client_.is_authorized()) {
        state_ = State::Done;
        return state_;
    }

    state_ = State::WaitingCode;

    // The login method handles the auth state machine internally
    bool success = client_.login(std::move(code_cb), std::move(pw_cb));
    state_ = success ? State::Done : State::Failed;
    return state_;
}

void AuthFlow::logout() {
    client_.logout();
    state_ = State::Idle;

    // Clear saved credentials
    auto& cfg = ConfigManager::instance();
    auto config = cfg.get();
    config.telegram.phone.clear();
    cfg.set(config);
    cfg.save();

    spdlog::info("Logged out successfully");
}

} // namespace tv
