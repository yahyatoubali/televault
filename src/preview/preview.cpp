#include "preview.hpp"
#include <spdlog/spdlog.h>
#include <array>
#include <unordered_map>
#include <cctype>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <ranges>
#include <filesystem>

namespace tv {

static const std::unordered_map<std::string, FileCategory> ext_category = {
    {".txt", FileCategory::Text},
    {".md", FileCategory::Text},
    {".json", FileCategory::Text},
    {".xml", FileCategory::Text},
    {".yaml", FileCategory::Text},
    {".yml", FileCategory::Text},
    {".csv", FileCategory::Text},
    {".log", FileCategory::Text},
    {".py", FileCategory::Text},
    {".cpp", FileCategory::Text},
    {".c", FileCategory::Text},
    {".h", FileCategory::Text},
    {".hpp", FileCategory::Text},
    {".js", FileCategory::Text},
    {".ts", FileCategory::Text},
    {".html", FileCategory::Text},
    {".css", FileCategory::Text},
    {".sh", FileCategory::Text},
    {".rs", FileCategory::Text},
    {".go", FileCategory::Text},
    {".toml", FileCategory::Text},
    {".ini", FileCategory::Text},
    {".cfg", FileCategory::Text},

    {".jpg", FileCategory::Image},
    {".jpeg", FileCategory::Image},
    {".png", FileCategory::Image},
    {".gif", FileCategory::Image},
    {".webp", FileCategory::Image},
    {".bmp", FileCategory::Image},
    {".svg", FileCategory::Image},

    {".mp4", FileCategory::Video},
    {".mkv", FileCategory::Video},
    {".avi", FileCategory::Video},
    {".mov", FileCategory::Video},
    {".webm", FileCategory::Video},

    {".mp3", FileCategory::Audio},
    {".m4a", FileCategory::Audio},
    {".ogg", FileCategory::Audio},
    {".opus", FileCategory::Audio},
    {".flac", FileCategory::Audio},
    {".wav", FileCategory::Audio},
    {".aac", FileCategory::Audio},
};

static const std::unordered_map<std::string, std::string> ext_mime = {
    {".txt", "text/plain"},
    {".md", "text/markdown"},
    {".json", "application/json"},
    {".xml", "application/xml"},
    {".csv", "text/csv"},
    {".html", "text/html"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".png", "image/png"},
    {".gif", "image/gif"},
    {".webp", "image/webp"},
    {".svg", "image/svg+xml"},
    {".mp4", "video/mp4"},
    {".mp3", "audio/mpeg"},
    {".pdf", "application/pdf"},
    {".zip", "application/zip"},
};

class PreviewEngine::Impl {
public:
    FileCategory classify(const std::string& filename) const {
        auto pos = filename.rfind('.');
        if (pos == std::string::npos) return FileCategory::Binary;

        auto ext = filename.substr(pos);
        std::string lower;
        lower.resize(ext.size());
        std::ranges::transform(ext, lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        auto it = ext_category.find(lower);
        return it != ext_category.end() ? it->second : FileCategory::Binary;
    }

    std::string get_mime(const std::string& filename) const {
        auto pos = filename.rfind('.');
        if (pos == std::string::npos) return "application/octet-stream";

        auto ext = filename.substr(pos);
        std::string lower;
        lower.resize(ext.size());
        std::ranges::transform(ext, lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        auto it = ext_mime.find(lower);
        return it != ext_mime.end() ? it->second : "application/octet-stream";
    }

    PreviewResult preview_file(const std::string& local_path, const std::string& original_filename = "") {
        PreviewResult result;
        std::string name_for_type = original_filename.empty() ? local_path : original_filename;
        result.category = classify(name_for_type);
        result.mime_type = get_mime(name_for_type);

        switch (result.category) {
            case FileCategory::Text:
                result.text_preview = preview_text(local_path);
                break;
            case FileCategory::Image:
                result.text_preview = preview_image(local_path);
                break;
            default:
                result.text_preview = preview_hex(local_path);
                break;
        }

        return result;
    }

private:
    std::string preview_text(const std::string& path, int max_lines = 30) {
        std::ifstream f(path);
        if (!f.is_open()) return "(cannot open)";

        std::ostringstream out;
        std::string line;
        int count = 0;
        while (std::getline(f, line) && count < max_lines) {
            out << line << "\n";
            ++count;
        }
        if (f.good()) out << "... (" << max_lines << " lines shown)\n";
        return out.str();
    }

    std::string preview_image(const std::string& path) {
        // Read image headers for basic metadata
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return "(cannot open)";

        auto size = std::filesystem::file_size(path);
        std::ostringstream out;
        out << "Image file\n";
        out << "Size: " << size << " bytes\n";

        // Check for PNG/JPEG headers
        std::array<char, 8> header{};
        f.read(header.data(), header.size());

        if (header[0] == char(0x89) && header[1] == 'P' && header[2] == 'N' && header[3] == 'G') {
            out << "Format: PNG\n";
        } else if (header[0] == char(0xFF) && header[1] == char(0xD8)) {
            out << "Format: JPEG\n";
        } else if (header[0] == 'G' && header[1] == 'I' && header[2] == 'F') {
            out << "Format: GIF\n";
        } else if (header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F') {
            out << "Format: WebP\n";
        }

        return out.str();
    }

    std::string preview_hex(const std::string& path, int max_bytes = 256) {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return "(cannot open)";

        std::vector<char> buf(max_bytes);
        auto read = f.read(buf.data(), buf.size()).gcount();

        std::ostringstream out;
        out << "Binary file (" << std::filesystem::file_size(path) << " bytes)\n\n";
        out << "Hex dump (first " << read << " bytes):\n";

        for (std::streamsize i = 0; i < read; i += 16) {
            auto remaining = std::min<std::streamsize>(16, read - i);
            out << std::hex << std::setfill('0') << std::setw(8) << i << "  ";

            for (std::streamsize j = 0; j < 16; ++j) {
                if (j < remaining) {
                    out << std::hex << std::setfill('0') << std::setw(2)
                        << (static_cast<int>(buf[i + j]) & 0xFF) << " ";
                } else {
                    out << "   ";
                }
                if (j == 7) out << " ";
            }

            out << " |";
            for (std::streamsize j = 0; j < remaining; ++j) {
                auto c = static_cast<unsigned char>(buf[i + j]);
                out << (std::isprint(c) ? static_cast<char>(c) : '.');
            }
            out << "|\n";
        }

        return out.str();
    }
};

PreviewEngine::PreviewEngine() : impl_(std::make_unique<Impl>()) {}
PreviewEngine::~PreviewEngine() = default;

FileCategory PreviewEngine::classify(const std::string& filename) const {
    return impl_->classify(filename);
}

std::string PreviewEngine::mime_type(const std::string& filename) const {
    return impl_->get_mime(filename);
}

PreviewResult PreviewEngine::preview(const std::string& vault_path, const std::string& original_filename) {
    return impl_->preview_file(vault_path, original_filename);
}

} // namespace tv
