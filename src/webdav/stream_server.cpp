#include "stream_server.hpp"
#include "../core/vault.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <spdlog/spdlog.h>
#include <thread>
#include <atomic>
#include <regex>
#include <filesystem>
#include <algorithm>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace tv {

namespace {

std::string guess_mime_type(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (ext == ".mp4") return "video/mp4";
    if (ext == ".mkv") return "video/x-matroska";
    if (ext == ".webm") return "video/webm";
    if (ext == ".avi") return "video/x-msvideo";
    if (ext == ".mov") return "video/quicktime";
    if (ext == ".mp3") return "audio/mpeg";
    if (ext == ".flac") return "audio/flac";
    if (ext == ".wav") return "audio/wav";
    if (ext == ".ogg") return "audio/ogg";
    if (ext == ".m4a") return "audio/mp4";
    if (ext == ".pdf") return "application/pdf";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".png") return "image/png";
    if (ext == ".gif") return "image/gif";
    return "application/octet-stream";
}

std::string url_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            int value = 0;
            std::istringstream is(std::string(in.substr(i + 1, 2)));
            if (is >> std::hex >> value) {
                out += static_cast<char>(value);
                i += 2;
            } else {
                out += in[i];
            }
        } else if (in[i] == '+') {
            out += ' ';
        } else {
            out += in[i];
        }
    }
    return out;
}

} // namespace

class StreamServer::Impl {
public:
    TeleVault& vault;
    StreamOptions opts_;
    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> thread_;
    net::io_context ioc_{1};
    std::unique_ptr<tcp::acceptor> acceptor_;

    explicit Impl(TeleVault& v) : vault(v) {}

    ~Impl() {
        stop();
    }

    bool start(const StreamOptions& opts, bool blocking) {
        if (running_) return true;
        opts_ = opts;

        try {
            auto const address = net::ip::make_address(opts_.host);
            acceptor_ = std::make_unique<tcp::acceptor>(ioc_, tcp::endpoint{address, opts_.port});
            acceptor_->set_option(net::socket_base::reuse_address(true));
        } catch (const std::exception& e) {
            spdlog::error("StreamServer failed to bind {}:{}: {}", opts_.host, opts_.port, e.what());
            return false;
        }

        running_ = true;
        spdlog::info("TeleVault HTTP Streaming Server started on http://{}:{}", opts_.host, opts_.port);

        if (blocking) {
            run_loop();
            return true;
        } else {
            thread_ = std::make_unique<std::thread>([this] { run_loop(); });
            return true;
        }
    }

    void stop() {
        if (!running_) return;
        running_ = false;

        beast::error_code ec;
        if (acceptor_) {
            acceptor_->close(ec);
        }
        ioc_.stop();

        if (thread_ && thread_->joinable()) {
            thread_->join();
        }
        thread_.reset();
    }

    void run_loop() {
        while (running_) {
            tcp::socket socket{ioc_};
            beast::error_code ec;
            acceptor_->accept(socket, ec);
            if (ec) {
                if (ec == net::error::operation_aborted) break;
                continue;
            }

            std::thread([this, s = std::move(socket)]() mutable {
                handle_connection(std::move(s));
            }).detach();
        }
    }

    void handle_connection(tcp::socket socket) {
        beast::error_code ec;
        beast::flat_buffer buffer;
        http::request<http::string_body> req;

        http::read(socket, buffer, req, ec);
        if (ec) return;

        try {
            serve_request(socket, req);
        } catch (const std::exception& e) {
            // Crypto/decode failures must surface as HTTP 500, never as an
            // uncaught exception that terminates the server process.
            spdlog::error("Stream request failed: {}", e.what());
            try {
                http::response<http::string_body> res{http::status::internal_server_error, req.version()};
                res.set(http::field::content_type, "text/plain");
                res.body() = std::string("Stream failed: ") + e.what();
                res.prepare_payload();
                http::write(socket, res, ec);
            } catch (...) {}
        }
    }

    void serve_request(tcp::socket& socket, const http::request<http::string_body>& req) {
        beast::error_code ec;
        std::string target(req.target());
        // Strip query string if any
        auto q_pos = target.find('?');
        if (q_pos != std::string::npos) target = target.substr(0, q_pos);

        // Strip prefix /stream/ or leading slash
        std::string filename = target;
        if (filename.starts_with("/stream/")) {
            filename = filename.substr(8);
        } else if (filename.starts_with("/")) {
            filename = filename.substr(1);
        }
        filename = url_decode(filename);

        auto file_info = vault.get_file_info(filename);
        if (!file_info) {
            http::response<http::string_body> res{http::status::not_found, req.version()};
            res.set(http::field::content_type, "text/plain");
            res.body() = "404 Not Found: File not found in vault: " + filename;
            res.prepare_payload();
            http::write(socket, res, ec);
            return;
        }

        uint64_t total_size = file_info->size;
        std::string mime = guess_mime_type(file_info->name);
        VaultOptions vopts;
        vopts.password = opts_.password;

        // Check Range header
        auto range_it = req.find(http::field::range);
        if (range_it != req.end()) {
            std::string range_val(range_it->value());
            std::regex range_regex(R"(bytes=(\d*)-(\d*))");
            std::smatch m;
            if (std::regex_match(range_val, m, range_regex)) {
                uint64_t start_byte = 0;
                uint64_t end_byte = (total_size > 0) ? total_size - 1 : 0;

                if (!m[1].str().empty()) {
                    start_byte = std::stoull(m[1].str());
                }
                if (!m[2].str().empty()) {
                    end_byte = std::stoull(m[2].str());
                }
                end_byte = std::min(end_byte, (total_size > 0) ? total_size - 1 : 0);

                if (start_byte > end_byte || start_byte >= total_size) {
                    http::response<http::string_body> res{http::status::range_not_satisfiable, req.version()};
                    res.set(http::field::content_range, std::format("bytes */{}", total_size));
                    res.prepare_payload();
                    http::write(socket, res, ec);
                    return;
                }

                auto chunk_bytes = vault.read_byte_range(file_info->name, start_byte, end_byte, vopts);
                if (!chunk_bytes) {
                    http::response<http::string_body> res{http::status::internal_server_error, req.version()};
                    res.body() = "Failed to decrypt requested byte range";
                    res.prepare_payload();
                    http::write(socket, res, ec);
                    return;
                }

                http::response<http::vector_body<uint8_t>> res{http::status::partial_content, req.version()};
                res.set(http::field::content_type, mime);
                res.set(http::field::accept_ranges, "bytes");
                res.set(http::field::content_range,
                        std::format("bytes {}-{}/{}", start_byte, start_byte + chunk_bytes->size() - 1, total_size));
                res.set(http::field::access_control_allow_origin, "*");
                res.body() = std::move(*chunk_bytes);
                res.prepare_payload();
                http::write(socket, res, ec);
                return;
            }
        }

        // Full file request (or initial probe)
        // Stream the first 16 MB or file content with Accept-Ranges
        uint64_t initial_len = std::min<uint64_t>(total_size, 16 * 1024 * 1024);
        auto initial_data = vault.read_byte_range(file_info->name, 0, initial_len > 0 ? initial_len - 1 : 0, vopts);

        if (!initial_data) {
            http::response<http::string_body> res{http::status::internal_server_error, req.version()};
            res.body() = "Failed to read file from vault";
            res.prepare_payload();
            http::write(socket, res, ec);
            return;
        }

        http::response<http::vector_body<uint8_t>> res{http::status::ok, req.version()};
        res.set(http::field::content_type, mime);
        res.set(http::field::accept_ranges, "bytes");
        // Body holds only the initial window (up to 16 MB); advertise its
        // real length, not total_size, or large-file clients hang waiting
        // for bytes that never arrive (they follow up with Range requests).
        res.set(http::field::content_length, std::to_string(initial_data->size()));
        res.set(http::field::access_control_allow_origin, "*");
        res.body() = std::move(*initial_data);
        res.prepare_payload();
        http::write(socket, res, ec);
    }
};

StreamServer::StreamServer(TeleVault& vault) : impl_(std::make_unique<Impl>(vault)) {}
StreamServer::~StreamServer() = default;

bool StreamServer::start(const StreamOptions& opts, bool blocking) {
    return impl_->start(opts, blocking);
}

void StreamServer::stop() {
    impl_->stop();
}

bool StreamServer::is_running() const noexcept {
    return impl_->running_;
}

uint16_t StreamServer::port() const noexcept {
    return impl_->opts_.port;
}

std::string StreamServer::stream_url(const std::string& filename) const {
    return std::format("http://{}:{}/stream/{}", impl_->opts_.host, impl_->opts_.port, filename);
}

} // namespace tv
