#include "s3_server.hpp"
#include "../core/vault.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <spdlog/spdlog.h>
#include <thread>
#include <atomic>
#include <sstream>
#include <regex>
#include <filesystem>
#include <fstream>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace tv {

namespace {

std::string s3_url_decode(std::string_view in) {
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

std::string s3_format_iso8601(std::chrono::system_clock::time_point tp) {
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm gmt{};
    gmtime_r(&tt, &gmt);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gmt);
    return std::string(buf);
}

} // anonymous namespace

class S3Server::Impl {
public:
    TeleVault& vault_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};
    net::io_context ioc_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    S3Options opts_;

    explicit Impl(TeleVault& vault) : vault_(vault) {}

    ~Impl() {
        stop();
    }

    bool start(const S3Options& opts) {
        if (running_) return true;
        opts_ = opts;

        try {
            auto const address = net::ip::make_address(opts.host);
            auto const port = opts.port;

            acceptor_ = std::make_unique<tcp::acceptor>(ioc_, tcp::endpoint{address, port});
            running_ = true;

            spdlog::info("TeleVault S3 gateway listening on http://{}:{}", opts.host, opts.port);

            server_thread_ = std::thread([this]() {
                run_loop();
            });

            return true;
        } catch (const std::exception& e) {
            spdlog::error("Failed to start S3 gateway: {}", e.what());
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
        spdlog::info("TeleVault S3 gateway stopped");
    }

    bool is_running() const {
        return running_.load();
    }

private:
    void run_loop() {
        while (running_) {
            boost::system::error_code ec;
            tcp::socket socket(ioc_);
            acceptor_->accept(socket, ec);

            if (ec) {
                if (ec == net::error::operation_aborted || !running_) break;
                spdlog::warn("S3 accept error: {}", ec.message());
                continue;
            }

            std::thread([this, s = std::move(socket)]() mutable {
                beast::tcp_stream stream(std::move(s));
                stream.expires_after(std::chrono::seconds(120));
                beast::flat_buffer buffer;

                for (;;) {
                    http::request<http::string_body> req;
                    boost::system::error_code ec;
                    http::read(stream, buffer, req, ec);

                    if (ec == http::error::end_of_stream || ec == net::error::operation_aborted) break;
                    if (ec) {
                        spdlog::debug("S3 read error: {}", ec.message());
                        break;
                    }

                    try {
                        auto res = handle_s3_request(req);
                        bool keep_alive = res.keep_alive();
                        http::write(stream, res, ec);

                        if (ec || !keep_alive) break;
                    } catch (const std::exception& e) {
                        // Malformed requests must not terminate the gateway.
                        spdlog::error("S3 request failed: {}", e.what());
                        try {
                            http::response<http::string_body> res{http::status::internal_server_error, req.version()};
                            res.set(http::field::content_type, "application/xml");
                            res.body() = std::string("<Error><Code>InternalError</Code><Message>") + e.what() + "</Message></Error>";
                            res.prepare_payload();
                            http::write(stream, res, ec);
                        } catch (...) {}
                        break;
                    }
                }
            }).detach();
        }
    }

    http::response<http::string_body> handle_s3_request(const http::request<http::string_body>& req) {
        std::string method(req.method_string());
        std::string target(req.target());

        auto q_pos = target.find('?');
        std::string path_only = (q_pos != std::string::npos) ? target.substr(0, q_pos) : target;
        path_only = s3_url_decode(path_only);

        while (!path_only.empty() && path_only.front() == '/') path_only.erase(path_only.begin());

        if (path_only.empty()) {
            // GET / -> ListAllMyBuckets
            if (method == "GET") {
                std::ostringstream xml;
                xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
                xml << "<ListAllMyBucketsResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n";
                xml << "  <Owner><ID>televault</ID><DisplayName>televault</DisplayName></Owner>\n";
                xml << "  <Buckets>\n";
                xml << "    <Bucket><Name>televault</Name><CreationDate>2026-01-01T00:00:00.000Z</CreationDate></Bucket>\n";
                xml << "  </Buckets>\n";
                xml << "</ListAllMyBucketsResult>\n";

                http::response<http::string_body> res{http::status::ok, req.version()};
                res.set(http::field::server, "TeleVault-S3/4.0.0");
                res.set(http::field::content_type, "application/xml");
                res.body() = xml.str();
                res.prepare_payload();
                return res;
            }
        }

        // Target path starting with televault bucket: televault/...
        std::string bucket = "televault";
        std::string key;

        if (path_only == bucket || path_only == bucket + "/") {
            key = "";
        } else if (path_only.starts_with(bucket + "/")) {
            key = path_only.substr(bucket.size() + 1);
        } else {
            // If path does not start with televault, treat entire path as key in televault bucket
            key = path_only;
        }

        if (key.empty()) {
            // List objects in bucket
            if (method == "GET") {
                auto files = vault_.list_files();
                std::ostringstream xml;
                xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
                xml << "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n";
                xml << "  <Name>televault</Name>\n";
                xml << "  <Prefix></Prefix>\n";
                xml << "  <KeyCount>" << files.size() << "</KeyCount>\n";
                xml << "  <MaxKeys>10000</MaxKeys>\n";
                xml << "  <IsTruncated>false</IsTruncated>\n";
                for (const auto& f : files) {
                    if (f.is_trashed) continue;
                    xml << "  <Contents>\n";
                    xml << "    <Key>" << f.name << "</Key>\n";
                    xml << "    <LastModified>" << s3_format_iso8601(f.created_at) << "</LastModified>\n";
                    xml << "    <ETag>\"" << f.hash << "\"</ETag>\n";
                    xml << "    <Size>" << f.size << "</Size>\n";
                    xml << "    <StorageClass>STANDARD</StorageClass>\n";
                    xml << "  </Contents>\n";
                }
                xml << "</ListBucketResult>\n";

                http::response<http::string_body> res{http::status::ok, req.version()};
                res.set(http::field::server, "TeleVault-S3/4.0.0");
                res.set(http::field::content_type, "application/xml");
                res.body() = xml.str();
                res.prepare_payload();
                return res;
            }
        }

        // Object operations
        if (method == "GET" || method == "HEAD") {
            auto meta = vault_.get_file_info(key);
            if (!meta || meta->is_trashed) {
                http::response<http::string_body> res{http::status::not_found, req.version()};
                res.set(http::field::server, "TeleVault-S3/4.0.0");
                res.set(http::field::content_type, "application/xml");
                res.body() = "<Error><Code>NoSuchKey</Code><Message>The specified key does not exist.</Message></Error>";
                res.prepare_payload();
                return res;
            }

            uint64_t total_size = meta->size;
            VaultOptions vopts;
            vopts.password = opts_.password;

            auto range_it = req.find(http::field::range);
            if (range_it != req.end()) {
                std::string range_val(range_it->value());
                std::regex range_regex(R"(bytes=(\d*)-(\d*))");
                std::smatch m;
                if (std::regex_match(range_val, m, range_regex)) {
                    uint64_t start = 0;
                    uint64_t end = (total_size > 0) ? total_size - 1 : 0;
                    auto parse_num = [](const std::string& s, uint64_t& out) -> bool {
                        if (s.empty() || s.size() > 19) return false;
                        for (char c : s) {
                            if (c < '0' || c > '9') return false;
                        }
                        try {
                            out = std::stoull(s);
                            return true;
                        } catch (...) {
                            return false;
                        }
                    };
                    if (!m[1].str().empty() && !parse_num(m[1].str(), start)) {
                        http::response<http::string_body> oor{http::status::range_not_satisfiable, req.version()};
                        oor.prepare_payload();
                        return oor;
                    }
                    if (!m[2].str().empty() && !parse_num(m[2].str(), end)) {
                        http::response<http::string_body> oor{http::status::range_not_satisfiable, req.version()};
                        oor.prepare_payload();
                        return oor;
                    }
                    end = std::min(end, (total_size > 0) ? total_size - 1 : 0);

                    auto chunk = vault_.read_byte_range(meta->name, start, end, vopts);
                    if (!chunk) {
                        http::response<http::string_body> err{http::status::internal_server_error, req.version()};
                        err.prepare_payload();
                        return err;
                    }

                    http::response<http::string_body> res{http::status::partial_content, req.version()};
                    res.set(http::field::server, "TeleVault-S3/4.0.0");
                    res.set(http::field::content_type, "application/octet-stream");
                    res.set(http::field::accept_ranges, "bytes");
                    res.set(http::field::content_range, std::format("bytes {}-{}/{}", start, start + chunk->size() - 1, total_size));
                    res.set(http::field::etag, std::format("\"{}\"", meta->hash));
                    if (method == "GET") {
                        res.body().assign(reinterpret_cast<const char*>(chunk->data()), chunk->size());
                    }
                    res.prepare_payload();
                    return res;
                }
            }

            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::server, "TeleVault-S3/4.0.0");
            res.set(http::field::content_type, "application/octet-stream");
            res.set(http::field::accept_ranges, "bytes");
            res.set(http::field::content_length, std::to_string(total_size));
            res.set(http::field::etag, std::format("\"{}\"", meta->hash));

            if (method == "GET" && total_size > 0) {
                auto all = vault_.read_byte_range(meta->name, 0, total_size - 1, vopts);
                if (all) {
                    res.body().assign(reinterpret_cast<const char*>(all->data()), all->size());
                }
            }
            res.prepare_payload();
            return res;
        }

        if (method == "PUT") {
            // Write payload to temp file and push
            auto tmp_path = std::filesystem::temp_directory_path() /
                std::format("tvt_s3_upload_{}_{}", std::rand(), std::filesystem::path(key).filename().string());

            {
                std::ofstream f(tmp_path, std::ios::binary);
                f.write(req.body().data(), req.body().size());
            }

            VaultOptions vopts;
            vopts.password = opts_.password;
            vopts.fastcdc = true;
            vopts.deduplicate = true;

            bool ok = vault_.push(tmp_path.string(), vopts);
            std::filesystem::remove(tmp_path);

            if (ok) {
                http::response<http::string_body> res{http::status::ok, req.version()};
                res.set(http::field::server, "TeleVault-S3/4.0.0");
                res.set(http::field::etag, "\"uploaded\"");
                res.set(http::field::content_length, "0");
                res.prepare_payload();
                return res;
            } else {
                http::response<http::string_body> res{http::status::internal_server_error, req.version()};
                res.body() = "<Error><Code>InternalError</Code><Message>Failed to upload to vault</Message></Error>";
                res.prepare_payload();
                return res;
            }
        }

        if (method == "DELETE") {
            vault_.delete_file(key, true);
            http::response<http::string_body> res{http::status::no_content, req.version()};
            res.set(http::field::server, "TeleVault-S3/4.0.0");
            res.prepare_payload();
            return res;
        }

        http::response<http::string_body> res{http::status::not_found, req.version()};
        res.prepare_payload();
        return res;
    }
};

S3Server::S3Server(TeleVault& vault) : impl_(std::make_unique<Impl>(vault)) {}
S3Server::~S3Server() = default;

bool S3Server::start(const S3Options& opts) {
    return impl_->start(opts);
}

void S3Server::stop() {
    impl_->stop();
}

bool S3Server::is_running() const {
    return impl_->is_running();
}

} // namespace tv
