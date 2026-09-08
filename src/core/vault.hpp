#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include "../models/file_metadata.hpp"
#include "../models/vault_index.hpp"

namespace tv {

class TelegramClient;

struct ProgressInfo {
    uint64_t current{};
    uint64_t total{};
    std::string stage;
    double speed{};
};

using ProgressCallback = std::function<void(const ProgressInfo&)>;

struct VaultOptions {
    bool encrypted{true};
    bool compressed{true};
    bool low_resource{};
    bool resume{};
    bool deduplicate{true};
    bool fastcdc{true};
    bool delta{false};
    bool purge{false};
    std::string password;
    std::vector<int64_t> channel_ids;
};

struct FileEntry {
    std::string id;
    std::string name;
    uint64_t size{};
    std::string hash;
    bool encrypted{};
    bool compressed{};
    int chunk_count{};
    bool is_trashed{false};
    int32_t version{1};
    std::chrono::system_clock::time_point created_at;
};

class IndexManager;

class TeleVault {
public:
    explicit TeleVault(TelegramClient& tg);
    ~TeleVault();

    TeleVault(const TeleVault&) = delete;
    TeleVault& operator=(const TeleVault&) = delete;

    bool initialize(int64_t channel_id, bool low_resource = false);
    void set_shards(std::vector<int64_t> shard_channel_ids);

    // File operations
    bool push(const std::string& local_path, const VaultOptions& opts, ProgressCallback cb = {});
    bool pull(const std::string& vault_path, const std::string& output_path,
              const VaultOptions& opts, ProgressCallback cb = {});
    bool cat(const std::string& vault_path, const VaultOptions& opts = {}, ProgressCallback cb = {});
    [[nodiscard]] std::optional<std::vector<uint8_t>> read_first_chunk(const std::string& vault_path, const VaultOptions& opts = {});
    [[nodiscard]] std::optional<std::vector<uint8_t>> read_byte_range(const std::string& vault_path, uint64_t start_byte, uint64_t end_byte, const VaultOptions& opts = {});

    // Listing & querying
    [[nodiscard]] std::vector<FileEntry> list_files() const;
    [[nodiscard]] std::vector<FileEntry> list_trash() const;
    [[nodiscard]] std::vector<FileEntry> find_files(const std::string& query) const;
    [[nodiscard]] std::vector<FileMetadata> find_all_matching(const std::string& query) const;
    [[nodiscard]] std::optional<FileMetadata> get_file_info(const std::string& path) const;

    // Management
    bool delete_file(const std::string& path, bool purge = false);
    bool restore_file(const std::string& path);
    bool empty_trash();
    bool verify_file(const std::string& path);
    bool recover_index();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv
