#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <optional>

namespace tv {

enum class FileCategory { Text, Image, Video, Audio, Binary, Unknown };

struct PreviewResult {
    FileCategory category{FileCategory::Unknown};
    std::string mime_type;
    std::string text_preview;
    std::vector<uint8_t> thumbnail_data;
    std::optional<std::string> exif_info;
};

class PreviewEngine {
public:
    PreviewEngine();
    ~PreviewEngine();

    [[nodiscard]] FileCategory classify(const std::string& filename) const;
    [[nodiscard]] std::string mime_type(const std::string& filename) const;
    PreviewResult preview(const std::string& vault_path);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv
