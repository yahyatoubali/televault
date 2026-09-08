#pragma once

#include <string>
#include <memory>
#include <cstdint>

namespace tv {

class TeleVault;

struct S3Options {
    std::string host{"127.0.0.1"};
    uint16_t port{9000};
    std::string password;
};

class S3Server {
public:
    explicit S3Server(TeleVault& vault);
    ~S3Server();

    bool start(const S3Options& opts);
    void stop();
    bool is_running() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv
