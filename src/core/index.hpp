#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <optional>
#include "../models/vault_index.hpp"

namespace tv {

class TelegramClient;

class IndexManager {
public:
    explicit IndexManager(TelegramClient& tg);

    bool load(int64_t chat_id);
    bool save(int64_t chat_id);

    void add_file(const std::string& file_id, int64_t metadata_msg_id);
    void remove_file(const std::string& file_id);
    std::optional<int64_t> get_metadata_msg_id(const std::string& file_id) const;
    [[nodiscard]] const VaultIndex& index() const { return index_; }

private:
    TelegramClient& tg_;
    VaultIndex index_;
    mutable std::mutex mutex_;

    int64_t find_pinned_message_id(int64_t chat_id) const;
    std::string serialize() const;
    bool deserialize(const std::string& json_str);
};

} // namespace tv
