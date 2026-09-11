#include "server.hpp"
#include "handler.hpp"
#include "../core/vault.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <spdlog/spdlog.h>
#include <thread>
#include <atomic>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace tv {

class WebDAVServer::Impl {
public:
    TeleVault& vault_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};
    net::io_context ioc_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    WebDAVOptions opts_;

    explicit Impl(TeleVault& vault) : vault_(vault) {}

    ~Impl() {
        stop();
    }

    bool start(const WebDAVOptions& opts) {
        if (running_) return true;
        opts_ = opts;

        try {
            auto const address = net::ip::make_address(opts.host);
            auto const port = opts.port;

            acceptor_ = std::make_unique<tcp::acceptor>(ioc_, tcp::endpoint{address, port});
            running_ = true;

            spdlog::info("TeleVault WebDAV server listening on http://{}:{}", opts.host, opts.port);

            server_thread_ = std::thread([this]() {
                run_loop();
            });

            return true;
        } catch (const std::exception& e) {
            spdlog::error("Failed to start WebDAV server: {}", e.what());
            running_ = false;
            return false;
        }
    }

    void stop() {
        if (!running_.exchange(false)) return;

        boost::system::error_code ec;
        if (acceptor_) {
            acceptor_->close(ec);
        }
        ioc_.stop();

        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        spdlog::info("TeleVault WebDAV server stopped");
    }

    bool is_running() const {
        return running_.load();
    }

private:
    void run_loop() {
        WebDAVHandler handler(vault_, opts_.password);

        while (running_) {
            boost::system::error_code ec;
            tcp::socket socket(ioc_);
            acceptor_->accept(socket, ec);

            if (ec) {
                if (ec == net::error::operation_aborted || !running_) break;
                spdlog::warn("WebDAV accept error: {}", ec.message());
                continue;
            }

            std::thread([s = std::move(socket), &handler]() mutable {
                beast::tcp_stream stream(std::move(s));
                stream.expires_after(std::chrono::seconds(60));
                beast::flat_buffer buffer;

                for (;;) {
                    http::request<http::string_body> req;
                    boost::system::error_code ec;
                    http::read(stream, buffer, req, ec);

                    if (ec == http::error::end_of_stream || ec == net::error::operation_aborted) break;
                    if (ec) {
                        spdlog::debug("WebDAV read error: {}", ec.message());
                        break;
                    }

                    try {
                        auto res = handler.handle(req);
                        bool keep_alive = res.keep_alive();
                        http::write(stream, res, ec);

                        if (ec || !keep_alive) break;
                    } catch (const std::exception& e) {
                        // Malformed requests (e.g. overflowing Range values)
                        // must not terminate the server via std::terminate.
                        spdlog::error("WebDAV request failed: {}", e.what());
                        try {
                            http::response<http::string_body> res{http::status::internal_server_error, req.version()};
                            res.set(http::field::content_type, "text/plain");
                            res.body() = std::string("Request failed: ") + e.what();
                            res.prepare_payload();
                            http::write(stream, res, ec);
                        } catch (...) {}
                        break;
                    }
                }
            }).detach();
        }
    }
};

WebDAVServer::WebDAVServer(TeleVault& vault)
    : impl_(std::make_unique<Impl>(vault)) {}

WebDAVServer::~WebDAVServer() = default;

bool WebDAVServer::start(const WebDAVOptions& opts) {
    return impl_->start(opts);
}

void WebDAVServer::stop() {
    impl_->stop();
}

bool WebDAVServer::is_running() const {
    return impl_->is_running();
}

} // namespace tv
