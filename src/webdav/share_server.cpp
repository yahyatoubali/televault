#include "share_server.hpp"
#include "../core/vault.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <spdlog/spdlog.h>
#include <openssl/rand.h>
#include <thread>
#include <atomic>
#include <regex>
#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace tv {

namespace {

std::string generate_random_token(size_t len = 16) {
    std::vector<unsigned char> buf(len / 2);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
        return "tvshare12345678";
    }
    std::ostringstream oss;
    for (auto b : buf) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

std::string guess_mime(const std::string& path) {
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
    if (ext == ".txt" || ext == ".md") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

std::string format_size_human(uint64_t bytes) {
    constexpr double KIB = 1024.0;
    constexpr double MIB = KIB * 1024.0;
    constexpr double GIB = MIB * 1024.0;
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    if (bytes >= GIB) ss << (bytes / GIB) << " GiB";
    else if (bytes >= MIB) ss << (bytes / MIB) << " MiB";
    else if (bytes >= KIB) ss << (bytes / KIB) << " KiB";
    else ss << bytes << " B";
    return ss.str();
}

std::string get_query_param(std::string_view target, std::string_view key) {
    auto q_pos = target.find('?');
    if (q_pos == std::string_view::npos) return "";
    auto query = target.substr(q_pos + 1);
    std::string needle = std::string(key) + "=";
    auto pos = query.find(needle);
    if (pos == std::string_view::npos) return "";
    auto val = query.substr(pos + needle.size());
    auto amp = val.find('&');
    if (amp != std::string_view::npos) val = val.substr(0, amp);
    return std::string(val);
}

} // namespace

class ShareServer::Impl {
public:
    TeleVault& vault;
    ShareOptions opts_;
    std::string token_;
    std::chrono::steady_clock::time_point start_time_;
    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> thread_;
    net::io_context ioc_{1};
    std::unique_ptr<tcp::acceptor> acceptor_;

    explicit Impl(TeleVault& v) : vault(v) {}

    ~Impl() {
        stop();
    }

    bool start(const ShareOptions& opts, bool blocking) {
        if (running_) return true;
        opts_ = opts;
        token_ = opts_.token.empty() ? generate_random_token(16) : opts_.token;
        start_time_ = std::chrono::steady_clock::now();

        try {
            auto const address = net::ip::make_address(opts_.host);
            acceptor_ = std::make_unique<tcp::acceptor>(ioc_, tcp::endpoint{address, opts_.port});
            acceptor_->set_option(net::socket_base::reuse_address(true));
        } catch (const std::exception& e) {
            spdlog::error("ShareServer failed to bind {}:{}: {}", opts_.host, opts_.port, e.what());
            return false;
        }

        running_ = true;
        spdlog::info("TeleVault Ephemeral Share started on {}", share_url());

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

    [[nodiscard]] std::string share_url() const {
        std::string host_str = (opts_.host == "0.0.0.0") ? "127.0.0.1" : opts_.host;
        return "http://" + host_str + ":" + std::to_string(opts_.port) + "/s/" + token_;
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

        if (opts_.expires_in.count() > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_);
            if (elapsed >= opts_.expires_in) {
                http::response<http::string_body> res{http::status::gone, req.version()};
                res.set(http::field::content_type, "text/html");
                res.body() = "<html><body style='font-family:sans-serif;text-align:center;padding:50px;background:#121214;color:#eee;'>"
                             "<h1>Link Expired</h1><p>This TeleVault ephemeral share link has expired.</p></body></html>";
                res.prepare_payload();
                http::write(socket, res, ec);
                return;
            }
        }

        std::string target(req.target());
        std::string path_prefix = "/s/" + token_;

        if (!target.starts_with(path_prefix)) {
            http::response<http::string_body> res{http::status::not_found, req.version()};
            res.set(http::field::content_type, "text/plain");
            res.body() = "404 Not Found";
            res.prepare_payload();
            http::write(socket, res, ec);
            return;
        }

        std::string subpath = target.substr(path_prefix.size());
        auto qpos = subpath.find('?');
        if (qpos != std::string::npos) {
            subpath = subpath.substr(0, qpos);
        }

        if (!opts_.pin.empty()) {
            std::string pin_param = get_query_param(target, "pin");
            auto pin_hdr = req.find("X-Pin");
            std::string header_pin = (pin_hdr != req.end()) ? std::string(pin_hdr->value()) : "";

            if (pin_param != opts_.pin && header_pin != opts_.pin) {
                http::response<http::string_body> res{http::status::ok, req.version()};
                res.set(http::field::content_type, "text/html; charset=utf-8");
                res.body() = R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Protected Vault Share</title>
<style>
  body { background: #0d1117; color: #c9d1d9; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; display: flex; align-items: center; justify-content: center; height: 100vh; margin: 0; }
  .box { background: #161b22; border: 1px solid #30363d; border-radius: 12px; padding: 32px; width: 340px; box-shadow: 0 8px 24px rgba(0,0,0,0.5); text-align: center; }
  h2 { margin-top: 0; color: #58a6ff; font-size: 20px; }
  p { font-size: 14px; color: #8b949e; margin-bottom: 20px; }
  input[type="password"] { width: 100%; box-sizing: border-box; padding: 10px; border-radius: 6px; border: 1px solid #30363d; background: #0d1117; color: #fff; font-size: 16px; margin-bottom: 16px; text-align: center; letter-spacing: 4px; }
  button { width: 100%; padding: 10px; background: #238636; border: none; border-radius: 6px; color: #fff; font-size: 15px; font-weight: 600; cursor: pointer; }
  button:hover { background: #2ea043; }
</style>
</head>
<body>
<div class="box">
  <h2>🔒 Protected Share</h2>
  <p>Please enter the security PIN to unlock this file.</p>
  <form method="GET">
    <input type="password" name="pin" placeholder="••••" autofocus required autocomplete="off" />
    <button type="submit">Unlock & View</button>
  </form>
</div>
</body>
</html>)";
                res.prepare_payload();
                http::write(socket, res, ec);
                return;
            }
        }

        auto file_info = vault.get_file_info(opts_.remote_path);
        if (!file_info) {
            http::response<http::string_body> res{http::status::not_found, req.version()};
            res.set(http::field::content_type, "text/plain");
            res.body() = "404 Not Found: File not found in vault: " + opts_.remote_path;
            res.prepare_payload();
            http::write(socket, res, ec);
            return;
        }

        if (subpath == "/download" || subpath == "/raw" || subpath == "/stream") {
            serve_file_content(socket, req, *file_info, subpath == "/download");
            return;
        }

        std::string mime = guess_mime(file_info->name);
        bool is_video = mime.starts_with("video/");
        bool is_audio = mime.starts_with("audio/");
        std::string pin_suffix = opts_.pin.empty() ? "" : ("?pin=" + opts_.pin);

        std::string time_remaining_str = "No expiry";
        if (opts_.expires_in.count() > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_);
            auto remaining = opts_.expires_in - elapsed;
            if (remaining.count() > 0) {
                long mins = remaining.count() / 60;
                long secs = remaining.count() % 60;
                time_remaining_str = std::to_string(mins) + "m " + std::to_string(secs) + "s";
            }
        }

        std::ostringstream html;
        html << "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
        html << "<title>" << file_info->name << " - TeleVault Share</title>";
        html << R"(<style>
body { background: #0d1117; color: #c9d1d9; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; margin: 0; padding: 40px 20px; display: flex; flex-direction: column; align-items: center; }
.card { background: #161b22; border: 1px solid #30363d; border-radius: 12px; padding: 32px; max-width: 650px; width: 100%; box-shadow: 0 8px 24px rgba(0,0,0,0.5); }
.header { display: flex; align-items: center; gap: 12px; margin-bottom: 24px; border-bottom: 1px solid #21262d; padding-bottom: 16px; }
.icon { font-size: 32px; }
.title { font-size: 18px; font-weight: 600; color: #f0f6fc; word-break: break-all; }
.meta { font-size: 13px; color: #8b949e; display: flex; gap: 16px; margin-bottom: 24px; }
.player { width: 100%; border-radius: 8px; margin-bottom: 24px; background: #000; }
.actions { display: flex; gap: 12px; }
.btn { display: inline-flex; align-items: center; justify-content: center; padding: 12px 24px; border-radius: 6px; font-size: 15px; font-weight: 600; text-decoration: none; cursor: pointer; }
.btn-primary { background: #238636; color: #fff; border: 1px solid #2ea043; flex: 1; }
.btn-primary:hover { background: #2ea043; }
.btn-secondary { background: #21262d; color: #c9d1d9; border: 1px solid #30363d; }
.btn-secondary:hover { background: #30363d; }
</style></head><body>)";

        html << "<div class='card'>";
        html << "<div class='header'><span class='icon'>" << (is_video ? "🎬" : (is_audio ? "🎵" : "📄")) << "</span>";
        html << "<div><div class='title'>" << file_info->name << "</div>";
        html << "<div class='meta'><span>📦 " << format_size_human(file_info->size) << "</span>";
        html << "<span>⏳ " << time_remaining_str << "</span></div></div></div>";

        if (is_video) {
            html << "<video class='player' controls preload='metadata' src='" << path_prefix << "/stream" << pin_suffix << "'></video>";
        } else if (is_audio) {
            html << "<audio class='player' controls preload='metadata' src='" << path_prefix << "/stream" << pin_suffix << "'></audio>";
        }

        html << "<div class='actions'>";
        html << "<a class='btn btn-primary' href='" << path_prefix << "/download" << pin_suffix << "'>⬇️ Download File</a>";
        html << "<a class='btn btn-secondary' href='" << path_prefix << "/raw" << pin_suffix << "'>View Raw</a>";
        html << "</div></div></body></html>";

        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::content_type, "text/html; charset=utf-8");
        res.body() = html.str();
        res.prepare_payload();
        http::write(socket, res, ec);
    }

    void serve_file_content(tcp::socket& socket, const http::request<http::string_body>& req, const FileMetadata& file_info, bool download_attachment) {
        beast::error_code ec;
        uint64_t total_size = file_info.size;
        std::string mime = guess_mime(file_info.name);
        VaultOptions vopts;

        auto range_it = req.find(http::field::range);
        if (range_it != req.end()) {
            std::string range_val(range_it->value());
            std::regex range_regex(R"(bytes=(\d*)-(\d*))");
            std::smatch match;

            if (std::regex_search(range_val, match, range_regex)) {
                uint64_t start = 0;
                uint64_t end = (total_size > 0) ? total_size - 1 : 0;

                if (!match[1].str().empty()) {
                    start = std::stoull(match[1].str());
                }
                if (!match[2].str().empty()) {
                    end = std::stoull(match[2].str());
                }

                if (start > end || start >= total_size) {
                    http::response<http::string_body> res{http::status::range_not_satisfiable, req.version()};
                    res.set(http::field::content_range, "bytes */" + std::to_string(total_size));
                    res.prepare_payload();
                    http::write(socket, res, ec);
                    return;
                }

                uint64_t length = end - start + 1;
                constexpr uint64_t MAX_CHUNK = 8 * 1024 * 1024; // 8MB limit
                if (length > MAX_CHUNK) {
                    length = MAX_CHUNK;
                    end = start + length - 1;
                }

                auto chunk_res = vault.read_byte_range(opts_.remote_path, start, length, vopts);
                if (!chunk_res) {
                    http::response<http::string_body> res{http::status::internal_server_error, req.version()};
                    res.body() = "Failed to read byte range from vault";
                    res.prepare_payload();
                    http::write(socket, res, ec);
                    return;
                }

                http::response<http::vector_body<uint8_t>> res{http::status::partial_content, req.version()};
                res.set(http::field::content_type, mime);
                res.set(http::field::accept_ranges, "bytes");
                res.set(http::field::content_range, "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(total_size));
                if (download_attachment) {
                    res.set(http::field::content_disposition, "attachment; filename="" + file_info.name + """);
                }
                res.body() = std::move(*chunk_res);
                res.prepare_payload();
                http::write(socket, res, ec);
                return;
            }
        }

        if (total_size <= 16 * 1024 * 1024) {
            auto chunk_res = vault.read_byte_range(opts_.remote_path, 0, total_size, vopts);
            if (chunk_res) {
                http::response<http::vector_body<uint8_t>> res{http::status::ok, req.version()};
                res.set(http::field::content_type, mime);
                res.set(http::field::accept_ranges, "bytes");
                if (download_attachment) {
                    res.set(http::field::content_disposition, "attachment; filename="" + file_info.name + """);
                }
                res.body() = std::move(*chunk_res);
                res.prepare_payload();
                http::write(socket, res, ec);
                return;
            }
        }

        uint64_t chunk_len = std::min(total_size, static_cast<uint64_t>(8 * 1024 * 1024));
        auto chunk_res = vault.read_byte_range(opts_.remote_path, 0, chunk_len, vopts);
        if (!chunk_res) {
            http::response<http::string_body> res{http::status::internal_server_error, req.version()};
            res.body() = "Failed to read file from vault";
            res.prepare_payload();
            http::write(socket, res, ec);
            return;
        }

        http::response<http::vector_body<uint8_t>> res{http::status::ok, req.version()};
        res.set(http::field::content_type, mime);
        res.set(http::field::accept_ranges, "bytes");
        if (download_attachment) {
            res.set(http::field::content_disposition, "attachment; filename="" + file_info.name + """);
        }
        res.body() = std::move(*chunk_res);
        res.prepare_payload();
        http::write(socket, res, ec);
    }
};

ShareServer::ShareServer(TeleVault& vault) : impl_(std::make_unique<Impl>(vault)) {}
ShareServer::~ShareServer() = default;

bool ShareServer::start(const ShareOptions& opts, bool blocking) {
    return impl_->start(opts, blocking);
}

void ShareServer::stop() {
    impl_->stop();
}

bool ShareServer::is_running() const noexcept {
    return impl_->running_;
}

uint16_t ShareServer::port() const noexcept {
    return impl_->opts_.port;
}

std::string ShareServer::share_url() const {
    return impl_->share_url();
}

std::string ShareServer::token() const {
    return impl_->token_;
}

} // namespace tv
