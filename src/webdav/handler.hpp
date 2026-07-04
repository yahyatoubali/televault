#pragma once

#include <string>
#include <memory>
#include <boost/beast/http.hpp>

namespace tv {

class TeleVault;

namespace http = boost::beast::http;

class WebDAVHandler {
public:
    explicit WebDAVHandler(TeleVault& vault);

    http::response<http::string_body> handle(const http::request<http::string_body>& req);
    http::response<http::dynamic_body> handle_body(const http::request<http::string_body>& req);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv
