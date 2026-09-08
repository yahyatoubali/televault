#pragma once

#include <string>
#include <memory>
#include <cstdint>
#include <functional>

namespace tv {

class TeleVault;
struct VaultOptions;

struct StreamOptions {
    std::string host{"127.0.0.1"};
    uint16_t port{8080};
    std::string password;
};

class StreamServer {
public:
    explicit StreamServer(TeleVault& vault);
    ~StreamServer();

    StreamServer(const StreamServer&) = delete;
    StreamServer& operator=(const StreamServer&) = delete;

    /// Starts the streaming server. If blocking is true, blocks until stop() or SIGINT.
    bool start(const StreamOptions& opts, bool blocking = false);

    /// Stops the server
    void stop();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] uint16_t port() const noexcept;
    [[nodiscard]] std::string stream_url(const std::string& filename) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv
