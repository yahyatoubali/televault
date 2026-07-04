#include "server.hpp"
#include <spdlog/spdlog.h>

namespace tv {

class WebDAVServer::Impl {};

WebDAVServer::WebDAVServer(TeleVault&) : impl_(std::make_unique<Impl>()) {}
WebDAVServer::~WebDAVServer() = default;

bool WebDAVServer::start(const WebDAVOptions&) {
    spdlog::warn("WebDAVServer::start not yet implemented (Boost.Beast needed)");
    return false;
}

void WebDAVServer::stop() {}
bool WebDAVServer::is_running() const { return false; }

} // namespace tv
