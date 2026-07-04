#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>

namespace tv {

struct TransferOptions {
    int parallel_count{8};
    bool resume{};
    bool low_resource{};
    std::string password;
};

struct TransferProgress {
    int64_t file_id{};
    uint64_t total_bytes{};
    uint64_t transferred_bytes{};
    int completed_chunks{};
    int total_chunks{};
    double speed{};
};

class TransferManager {
public:
    explicit TransferManager(const TransferOptions& opts = {});

    void upload_file(const std::string& local_path,
                     std::function<bool(int64_t, std::vector<uint8_t>)> upload_fn,
                     std::function<void(const TransferProgress&)> progress_cb);

    void download_file(const std::string& output_path,
                       int total_chunks,
                       std::function<std::vector<uint8_t>(int64_t)> download_fn,
                       std::function<void(const TransferProgress&)> progress_cb);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv
