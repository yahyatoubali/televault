#pragma once

#include <string>
#include <functional>
#include <memory>

namespace tv {

class TelegramClient;

using CodeCallback = std::function<std::string()>;
using PasswordCallback = std::function<std::string()>;

class AuthFlow {
public:
    enum class State { Idle, Connecting, WaitingCode, WaitingPassword, Done, Failed };

    explicit AuthFlow(TelegramClient& client);
    ~AuthFlow() = default;

    State execute(const std::string& phone, CodeCallback code_cb, PasswordCallback pw_cb);
    void logout();
    State state() const { return state_; }

private:
    TelegramClient& client_;
    State state_{State::Idle};
};

} // namespace tv
