#pragma once

#include <string>
#include <memory>
#include <cstdint>
#include <chrono>

namespace tv {

class TeleVault;

struct ShareOptions {
    std::string remote_path;
    std::string host{"0.0.0.0"};
    uint16_t port{8080};
    std::string pin;
    std::chrono::seconds expires_in{3600}; // default 1 hour (0 = no expiry)
    std::string token;                     // if empty, auto-generated
    std::string password;                  // vault decryption password (required for encrypted files)
};

class ShareServer {
public:
    explicit ShareServer(TeleVault& vault);
    ~ShareServer();

    ShareServer(const ShareServer&) = delete;
    ShareServer& operator=(const ShareServer&) = delete;

    /// Start sharing server. If blocking is true, runs until stop() or SIGINT.
    bool start(const ShareOptions& opts, bool blocking = false);
    void stop();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] uint16_t port() const noexcept;
    [[nodiscard]] std::string share_url() const;
    [[nodiscard]] std::string token() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv
