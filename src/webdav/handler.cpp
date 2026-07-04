#include "handler.hpp"
#include <spdlog/spdlog.h>

namespace tv {

class WebDAVHandler::Impl {};

WebDAVHandler::WebDAVHandler(TeleVault&) : impl_(std::make_unique<Impl>()) {}

http::response<http::string_body> WebDAVHandler::handle(const http::request<http::string_body>&) {
    spdlog::warn("WebDAVHandler::handle not yet implemented");
    http::response<http::string_body> res{http::status::not_implemented, 11};
    res.body() = "Not implemented";
    res.prepare_payload();
    return res;
}

http::response<http::dynamic_body> WebDAVHandler::handle_body(const http::request<http::string_body>&) {
    spdlog::warn("WebDAVHandler::handle_body not yet implemented");
    http::response<http::dynamic_body> res{http::status::not_implemented, 11};
    res.prepare_payload();
    return res;
}

} // namespace tv
