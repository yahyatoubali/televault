#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <optional>
#include <cstdint>
#include <span>
#include <mutex>
#include <atomic>

namespace tv {

struct MessageInfo {
    int64_t id{};
    std::string text;
    int64_t reply_to_msg_id{};
    int32_t file_id{}; // tdlib file ID if message has a document
    std::string file_local_path; // local path if downloaded
};

struct FileInfo {
    int32_t file_id{};
    uint64_t size{};
    uint64_t downloaded_size{};
    std::string local_path;
};

using AuthCodeCallback = std::function<std::string()>;
using AuthPasswordCallback = std::function<std::string()>;
using FileProgressCallback = std::function<void(int32_t file_id, uint64_t downloaded, uint64_t total)>;

class TelegramClient {
public:
    TelegramClient();
    ~TelegramClient();

    TelegramClient(const TelegramClient&) = delete;
    TelegramClient& operator=(const TelegramClient&) = delete;

    // Connection & auth
    bool connect();
    bool is_authorized() const;
    bool login(AuthCodeCallback code_cb, AuthPasswordCallback pw_cb);
    void logout();
    int32_t api_id() const;
    std::string api_hash() const;

    // Channel operations
    bool create_channel(const std::string& title);
    bool set_channel(int64_t channel_id);
    bool is_valid_channel(int64_t channel_id) const;
    int64_t get_channel_id() const;

    // Message operations
    int64_t send_text(int64_t chat_id, const std::string& text, int64_t reply_to = 0);
    int64_t send_file(int64_t chat_id, const std::string& file_path, int64_t reply_to = 0);
    bool edit_message(int64_t chat_id, int64_t msg_id, const std::string& text);
    bool delete_messages(int64_t chat_id, const std::vector<int64_t>& msg_ids);
    MessageInfo get_message(int64_t chat_id, int64_t msg_id) const;
    std::vector<MessageInfo> get_chat_history(int64_t chat_id, int64_t from_msg_id = 0,
                                               int limit = 100) const;
    int64_t get_pinned_message_id(int64_t chat_id) const;
    bool pin_message(int64_t chat_id, int64_t msg_id);

    // File operations
    bool download_file(int32_t file_id, FileProgressCallback cb = {}) const;
    bool download_file_by_message(int64_t chat_id, int64_t msg_id, FileProgressCallback cb = {}) const;
    std::optional<FileInfo> get_file_info(int32_t file_id) const;
    struct FileSendResult {
        int64_t message_id{};
        int32_t file_id{};
    };
    FileSendResult send_file_with_id(int64_t chat_id, const std::string& file_path, int64_t reply_to = 0);

    // Whoami
    std::string get_my_username() const;
    int64_t get_my_id() const;

    // Tdlib parameters
    void set_api_params(int32_t api_id, const std::string& api_hash);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv
