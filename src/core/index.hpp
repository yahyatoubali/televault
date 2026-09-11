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
    bool sync(int64_t chat_id);

    void add_file(const std::string& file_id, int64_t metadata_msg_id);
    void remove_file(const std::string& file_id);
    std::optional<int64_t> get_metadata_msg_id(const std::string& file_id) const;
    [[nodiscard]] const VaultIndex& index() const { return index_; }
    [[nodiscard]] int64_t index_message_id() const { return index_message_id_; }

private:
    TelegramClient& tg_;
    VaultIndex index_;
    int64_t index_message_id_{0};
    mutable std::mutex mutex_;

    int64_t find_pinned_message_id(int64_t chat_id) const;
    bool sync_internal(int64_t chat_id);
    std::string serialize() const;
    bool deserialize(const std::string& json_str);
};

} // namespace tv
