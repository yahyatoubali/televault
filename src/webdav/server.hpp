#pragma once

#include <string>
#include <memory>
#include <cstdint>

namespace tv {

class TeleVault;

struct WebDAVOptions {
    std::string host{"127.0.0.1"};
    uint16_t port{8080};
    bool read_only{true};
    std::string password;
};

class WebDAVServer {
public:
    explicit WebDAVServer(TeleVault& vault);
    ~WebDAVServer();

    bool start(const WebDAVOptions& opts);
    void stop();
    bool is_running() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv
