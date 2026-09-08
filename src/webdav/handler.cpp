#include "handler.hpp"
#include "../core/vault.hpp"
#include <spdlog/spdlog.h>
#include <sstream>
#include <iomanip>
#include <regex>
#include <filesystem>
#include <chrono>

namespace tv {

namespace {

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

std::string format_rfc1123(std::chrono::system_clock::time_point tp) {
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm gmt{};
    gmtime_r(&tt, &gmt);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
    return std::string(buf);
}

std::string guess_mime(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ext == ".mp4") return "video/mp4";
    if (ext == ".mkv") return "video/x-matroska";
    if (ext == ".webm") return "video/webm";
    if (ext == ".mp3") return "audio/mpeg";
    if (ext == ".pdf") return "application/pdf";
    if (ext == ".txt" || ext == ".md") return "text/plain; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    return "application/octet-stream";
}

} // anonymous namespace

class WebDAVHandler::Impl {
public:
    TeleVault& vault_;
    std::string password_;

    Impl(TeleVault& vault, std::string password)
        : vault_(vault), password_(std::move(password)) {}

    http::response<http::string_body> handle(const http::request<http::string_body>& req) {
        std::string method_str(req.method_string());
        std::string target(req.target());

        // Strip query string
        auto q_pos = target.find('?');
        if (q_pos != std::string::npos) target = target.substr(0, q_pos);
        std::string decoded_path = url_decode(target);

        if (method_str == "OPTIONS") {
            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::server, "TeleVault-WebDAV/4.0.0");
            res.set(http::field::allow, "OPTIONS, GET, HEAD, PROPFIND");
            res.set("DAV", "1, 2");
            res.set("MS-Author-Via", "DAV");
            res.set(http::field::content_length, "0");
            res.prepare_payload();
            return res;
        }

        if (method_str == "PROPFIND") {
            return handle_propfind(req, decoded_path);
        }

        if (method_str == "GET" || method_str == "HEAD") {
            return handle_get_head(req, decoded_path, method_str == "HEAD");
        }

        http::response<http::string_body> res{http::status::method_not_allowed, req.version()};
        res.set(http::field::server, "TeleVault-WebDAV/4.0.0");
        res.set(http::field::allow, "OPTIONS, GET, HEAD, PROPFIND");
        res.body() = "Method Not Allowed";
        res.prepare_payload();
        return res;
    }

private:
    http::response<http::string_body> handle_propfind(
        const http::request<http::string_body>& req, const std::string& path)
    {
        std::string clean = path;
        while (!clean.empty() && clean.front() == '/') clean.erase(clean.begin());
        while (!clean.empty() && clean.back() == '/') clean.pop_back();

        auto files = vault_.list_files();
        std::ostringstream xml;
        xml << "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n";
        xml << "<D:multistatus xmlns:D=\"DAV:\">\n";

        // Root / requested collection
        xml << "  <D:response>\n";
        xml << "    <D:href>" << (path.empty() ? "/" : path) << "</D:href>\n";
        xml << "    <D:propstat>\n";
        xml << "      <D:prop>\n";
        xml << "        <D:displayname>" << (clean.empty() ? "/" : clean) << "</D:displayname>\n";
        xml << "        <D:resourcetype><D:collection/></D:resourcetype>\n";
        xml << "      </D:prop>\n";
        xml << "      <D:status>HTTP/1.1 200 OK</D:status>\n";
        xml << "    </D:propstat>\n";
        xml << "  </D:response>\n";

        // List files under directory
        for (const auto& f : files) {
            if (f.is_trashed) continue;
            if (!clean.empty() && !f.name.starts_with(clean + "/")) continue;

            xml << "  <D:response>\n";
            xml << "    <D:href>/" << f.name << "</D:href>\n";
            xml << "    <D:propstat>\n";
            xml << "      <D:prop>\n";
            xml << "        <D:displayname>" << std::filesystem::path(f.name).filename().string() << "</D:displayname>\n";
            xml << "        <D:getcontentlength>" << f.size << "</D:getcontentlength>\n";
            xml << "        <D:getcontenttype>" << guess_mime(f.name) << "</D:getcontenttype>\n";
            xml << "        <D:getlastmodified>" << format_rfc1123(f.created_at) << "</D:getlastmodified>\n";
            xml << "        <D:resourcetype/>\n";
            xml << "      </D:prop>\n";
            xml << "      <D:status>HTTP/1.1 200 OK</D:status>\n";
            xml << "    </D:propstat>\n";
            xml << "  </D:response>\n";
        }
        xml << "</D:multistatus>\n";

        http::response<http::string_body> res{http::status::multi_status, req.version()};
        res.set(http::field::server, "TeleVault-WebDAV/4.0.0");
        res.set(http::field::content_type, "application/xml; charset=\"utf-8\"");
        res.set("DAV", "1, 2");
        res.body() = xml.str();
        res.prepare_payload();
        return res;
    }

    http::response<http::string_body> handle_get_head(
        const http::request<http::string_body>& req, const std::string& path, bool head_only)
    {
        std::string clean = path;
        while (!clean.empty() && clean.front() == '/') clean.erase(clean.begin());

        if (clean.empty()) {
            // Root index HTML
            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "text/html; charset=utf-8");
            res.body() = "<html><body><h1>TeleVault WebDAV</h1><p>WebDAV endpoint active.</p></body></html>";
            if (head_only) res.body().clear();
            res.prepare_payload();
            return res;
        }

        auto meta = vault_.get_file_info(clean);
        if (!meta || meta->is_trashed) {
            http::response<http::string_body> res{http::status::not_found, req.version()};
            res.set(http::field::content_type, "text/plain");
            res.body() = "404 Not Found: " + clean;
            res.prepare_payload();
            return res;
        }

        uint64_t total_size = meta->size;
        std::string mime = guess_mime(meta->name);
        VaultOptions vopts;
        vopts.password = password_;

        auto range_it = req.find(http::field::range);
        if (range_it != req.end()) {
            std::string range_val(range_it->value());
            std::regex range_regex(R"(bytes=(\d*)-(\d*))");
            std::smatch m;
            if (std::regex_match(range_val, m, range_regex)) {
                uint64_t start_byte = 0;
                uint64_t end_byte = (total_size > 0) ? total_size - 1 : 0;
                if (!m[1].str().empty()) start_byte = std::stoull(m[1].str());
                if (!m[2].str().empty()) end_byte = std::stoull(m[2].str());
                end_byte = std::min(end_byte, (total_size > 0) ? total_size - 1 : 0);

                if (start_byte > end_byte || start_byte >= total_size) {
                    http::response<http::string_body> res{http::status::range_not_satisfiable, req.version()};
                    res.set(http::field::content_range, std::format("bytes */{}", total_size));
                    res.prepare_payload();
                    return res;
                }

                auto chunk_bytes = vault_.read_byte_range(meta->name, start_byte, end_byte, vopts);
                if (!chunk_bytes) {
                    http::response<http::string_body> res{http::status::internal_server_error, req.version()};
                    res.body() = "Decryption error for byte range";
                    res.prepare_payload();
                    return res;
                }

                http::response<http::string_body> res{http::status::partial_content, req.version()};
                res.set(http::field::content_type, mime);
                res.set(http::field::accept_ranges, "bytes");
                res.set(http::field::content_range,
                        std::format("bytes {}-{}/{}", start_byte, start_byte + chunk_bytes->size() - 1, total_size));
                if (!head_only) {
                    res.body().assign(reinterpret_cast<const char*>(chunk_bytes->data()), chunk_bytes->size());
                }
                res.prepare_payload();
                return res;
            }
        }

        // Full file download
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::content_type, mime);
        res.set(http::field::accept_ranges, "bytes");
        res.set(http::field::content_length, std::to_string(total_size));

        if (!head_only && total_size > 0) {
            auto all_bytes = vault_.read_byte_range(meta->name, 0, total_size - 1, vopts);
            if (!all_bytes) {
                http::response<http::string_body> err{http::status::internal_server_error, req.version()};
                err.body() = "Decryption error";
                err.prepare_payload();
                return err;
            }
            res.body().assign(reinterpret_cast<const char*>(all_bytes->data()), all_bytes->size());
        }
        res.prepare_payload();
        return res;
    }
};

WebDAVHandler::WebDAVHandler(TeleVault& vault, std::string password)
    : impl_(std::make_unique<Impl>(vault, std::move(password))) {}

WebDAVHandler::~WebDAVHandler() = default;

http::response<http::string_body> WebDAVHandler::handle(const http::request<http::string_body>& req) {
    return impl_->handle(req);
}

void WebDAVHandler::set_password(std::string password) {
    impl_->password_ = std::move(password);
}

} // namespace tv
