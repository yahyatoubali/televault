#include "client.hpp"
#include "session.hpp"
#include "qr.hpp"
#include "../util/logging.hpp"
#include "../util/config.hpp"

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>

#include <thread>
#include <mutex>
#include <filesystem>
#include <condition_variable>
#include <map>
#include <future>
#include <queue>
#include <atomic>
#include <chrono>
#include <print>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <zlib.h>

namespace tv {

namespace tda = ::td::td_api; // alias for shorter names
using tda::make_object;

// ── Helper: normalize message ID to TDLib format (server_id << 20) ────
static int64_t to_tdlib_msg_id(int64_t id) {
    if (id <= 0) return 0;
    if (id < (1LL << 20)) {
        return id << 20;
    }
    // Mask off temporary send bits in low 20 bits
    return (id >> 20) << 20;
}

// ── Helper: extract file_id from a message with document ─────────────
static int32_t extract_file_id(const tda::message& msg) {
    if (msg.content_->get_id() == tda::messageDocument::ID) {
        auto& doc = static_cast<const tda::messageDocument&>(*msg.content_);
        if (doc.document_ && doc.document_->document_) {
            return doc.document_->document_->id_;
        }
    }
    return 0;
}

static std::string extract_text(const tda::message& msg) {
    if (!msg.content_) return {};
    spdlog::debug("extract_text for msg {}: content_id={}", msg.id_, msg.content_->get_id());
    std::string raw;
    if (msg.content_->get_id() == tda::messageText::ID) {
        if (auto& mt = static_cast<const tda::messageText&>(*msg.content_); mt.text_) {
            raw = mt.text_->text_;
        }
    } else if (msg.content_->get_id() == tda::messageDocument::ID) {
        auto& doc = static_cast<const tda::messageDocument&>(*msg.content_);
        if (doc.caption_) {
            raw = doc.caption_->text_;
        }
    }
    if (raw.empty() || !raw.starts_with("__TV1__")) {
        return raw;
    }

    // Python __TV1__ format: base64-encoded zlib stream
    std::string b64 = raw.substr(7);
    BIO* bio = BIO_new_mem_buf(b64.data(), static_cast<int>(b64.size()));
    BIO* b64_bio = BIO_new(BIO_f_base64());
    BIO_set_flags(b64_bio, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64_bio, bio);

    std::vector<uint8_t> compressed(b64.size());
    int decoded_len = BIO_read(bio, compressed.data(), static_cast<int>(compressed.size()));
    BIO_free_all(bio);

    if (decoded_len <= 0) return raw;
    compressed.resize(decoded_len);

    z_stream strm{};
    if (inflateInit(&strm) != Z_OK) return raw;

    strm.next_in = compressed.data();
    strm.avail_in = static_cast<uInt>(compressed.size());

    std::string out;
    char buffer[4096];
    int ret;
    do {
        strm.next_out = reinterpret_cast<Bytef*>(buffer);
        strm.avail_out = sizeof(buffer);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&strm);
            return raw;
        }
        out.append(buffer, sizeof(buffer) - strm.avail_out);
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);
    return out;
}

static std::string compress_tv1(const std::string& text) {
    if (text.empty()) return text;

    z_stream strm{};
    if (deflateInit2(&strm, 9, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return text;
    }

    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(text.data()));
    strm.avail_in = static_cast<uInt>(text.size());

    std::vector<uint8_t> compressed;
    compressed.resize(deflateBound(&strm, static_cast<uLong>(text.size())));

    strm.next_out = compressed.data();
    strm.avail_out = static_cast<uInt>(compressed.size());

    int ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&strm);
        return text;
    }
    compressed.resize(compressed.size() - strm.avail_out);
    deflateEnd(&strm);

    // Base64 encode
    BIO* b64_bio = BIO_new(BIO_f_base64());
    BIO_set_flags(b64_bio, BIO_FLAGS_BASE64_NO_NL);
    BIO* mem_bio = BIO_new(BIO_s_mem());
    BIO* bio = BIO_push(b64_bio, mem_bio);

    BIO_write(bio, compressed.data(), static_cast<int>(compressed.size()));
    BIO_flush(bio);

    BUF_MEM* mem_ptr = nullptr;
    BIO_get_mem_ptr(bio, &mem_ptr);
    std::string result = "__TV1__";
    if (mem_ptr && mem_ptr->data && mem_ptr->length > 0) {
        result.append(mem_ptr->data, mem_ptr->length);
    }
    BIO_free_all(bio);
    return result;
}

static std::string maybe_compress(const std::string& text) {
    if (text.size() <= 4096) {
        return text;
    }
    auto comp = compress_tv1(text);
    if (comp.size() < text.size()) {
        return comp;
    }
    return text;
}

// ── Implementation ────────────────────────────────────────────────────
class TelegramClient::Impl {
public:
    Impl() = default;
    ~Impl() { stop(); }

    // ── Connection ──────────────────────────────────────────────────
    bool connect() {
        if (client_) return true;

        SessionManager sm;
        db_dir_ = sm.db_dir();
        files_dir_ = sm.files_dir();

        std::filesystem::create_directories(db_dir_);
        std::filesystem::create_directories(files_dir_);

        client_ = std::make_unique<::td::Client>();

        // Set up tdlib logging synchronously via execute()
        ::td::Client::execute({0, make_object<tda::setLogVerbosityLevel>(0)});
        ::td::Client::execute({0, make_object<tda::setLogStream>(
            make_object<tda::logStreamFile>(db_dir_ + "/tdlib.log", 1 << 24, false))});

        // Start client thread FIRST before any sends
        running_ = true;
        client_thread_ = std::thread([this] { client_loop(); });

        // Send initialization parameters
        auto params_obj = make_object<tda::tdlibParameters>();
        params_obj->use_test_dc_ = false;
        params_obj->database_directory_ = db_dir_;
        params_obj->files_directory_ = files_dir_;
        params_obj->use_file_database_ = true;
        params_obj->use_chat_info_database_ = true;
        params_obj->use_message_database_ = true;
        params_obj->use_secret_chats_ = false;
        params_obj->system_language_code_ = "en";
        params_obj->device_model_ = "Desktop";
        params_obj->application_version_ = TELEVAULT_VERSION;
        params_obj->api_id_ = api_id_;
        params_obj->api_hash_ = api_hash_;
        params_obj->enable_storage_optimizer_ = true;
        params_obj->ignore_file_names_ = false;
        auto params = make_object<tda::setTdlibParameters>(std::move(params_obj));

        // Send parameters synchronously so errors are caught immediately
        auto result = send_query_sync(std::move(params));
        if (!result || result->get_id() == tda::error::ID) {
            if (result) {
                auto& err = static_cast<tda::error&>(*result);
                spdlog::error("setTdlibParameters failed: {} (code {})", err.message_, err.code_);
                std::println("\033[31m✗ Telegram initialization error: {}\033[0m", err.message_);
            } else {
                spdlog::error("setTdlibParameters timed out");
                std::println("\033[31m✗ Telegram initialization timed out\033[0m");
            }
            stop();
            return false;
        }

        // Wait for initial authorization state
        {
            std::unique_lock lock(mutex_);
            if (!cv_.wait_for(lock, std::chrono::seconds(10), [this] {
                return auth_state_.load() >= AuthState::WaitPhone;
            })) {
                spdlog::error("Timed out waiting for Tdlib client to initialize (auth_state={})",
                    static_cast<int>(auth_state_.load()));
                stop();
                return false;
            }
        }

        ready_ = true;
        return true;
    }

    void stop() {
        running_ = false;
        if (client_thread_.joinable()) {
            client_thread_.join();
        }
        client_.reset();
        {
            std::lock_guard lock(mutex_);
            for (auto& [id, promise] : pending_) {
                try {
                    promise->set_value(ObjectPtr{});
                } catch (...) {}
            }
            pending_.clear();
        }
        {
            std::lock_guard lock(download_mutex_);
            for (auto& [id, promise] : pending_downloads_) {
                try {
                    promise->set_value(false);
                } catch (...) {}
            }
            pending_downloads_.clear();
        }
    }

    bool is_authorized() const {
        return auth_state_.load(std::memory_order_acquire) == AuthState::Ready;
    }

    // ── Authentication ──────────────────────────────────────────────
    bool login(AuthCodeCallback code_cb, AuthPasswordCallback pw_cb) {
        if (!ready_ && !connect()) return false;

        code_cb_ = std::move(code_cb);
        pw_cb_ = std::move(pw_cb);

        while (true) {
            AuthState current_state;
            {
                std::unique_lock lock(mutex_);
                if (!cv_.wait_for(lock, std::chrono::seconds(60), [this] {
                    return auth_state_.load() != AuthState::None;
                })) {
                    spdlog::error("Timed out waiting for Telegram authorization state update");
                    std::println("\033[31m✗ Timed out waiting for Telegram response\033[0m");
                    return false;
                }
                current_state = auth_state_.load();
            }

            switch (current_state) {
                case AuthState::WaitPhone:
                    if (!send_phone()) {
                        return false;
                    }
                    break;
                case AuthState::WaitCode:
                    if (!code_cb_) {
                        spdlog::error("Authentication code required but no callback provided");
                        return false;
                    }
                    if (!send_code(code_cb_())) {
                        return false;
                    }
                    break;
                case AuthState::WaitPassword:
                    if (!pw_cb_) {
                        spdlog::error("2FA password required but no callback provided");
                        std::println("\033[31m✗ 2FA password required\033[0m");
                        return false;
                    }
                    if (!send_password(pw_cb_())) {
                        return false;
                    }
                    break;
                case AuthState::WaitOtherDevice:
                    {
                        std::unique_lock lock(mutex_);
                        cv_.wait_for(lock, std::chrono::seconds(120), [this] {
                            return auth_state_.load() != AuthState::WaitOtherDevice;
                        });
                    }
                    break;
                case AuthState::Ready:
                    return true;
                case AuthState::Failed:
                    return false;
                default:
                    break;
            }
        }
    }

    // QR-code login: request the QR payload directly instead of going
    // through the phone-number/SMS path (which Telegram increasingly
    // rejects with UPDATE_APP_TO_LOGIN). The QR graphic itself is rendered
    // by handle_auth_state via print_login_qr().
    bool login_qr(AuthPasswordCallback pw_cb) {
        if (!ready_ && !connect()) return false;

        pw_cb_ = std::move(pw_cb);

        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(30), [this] {
                return auth_state_.load() != AuthState::None;
            });
            if (auth_state_.load() == AuthState::Ready) return true;
        }

        bool qr_requested = false;
        while (true) {
            AuthState current_state;
            {
                std::unique_lock lock(mutex_);
                if (!cv_.wait_for(lock, std::chrono::seconds(60), [this] {
                    return auth_state_.load() != AuthState::None;
                })) {
                    spdlog::error("Timed out waiting for Telegram authorization state update");
                    std::println("\033[31m✗ Timed out waiting for Telegram response\033[0m");
                    return false;
                }
                current_state = auth_state_.load();
            }

            switch (current_state) {
                case AuthState::WaitPhone:
                    if (!qr_requested) {
                        std::println("\033[1;36mRequesting QR code login...\033[0m");
                        if (!request_qr_code()) {
                            return false;
                        }
                        qr_requested = true;
                    }
                    break;
                case AuthState::WaitCode:
                    spdlog::error("Server asked for an SMS code during QR login");
                    std::println("\033[31m✗ Server unexpectedly asked for an SMS code; retry `tvt login --qr`\033[0m");
                    return false;
                case AuthState::WaitPassword:
                    if (!pw_cb_) {
                        spdlog::error("2FA password required but no callback provided");
                        std::println("\033[31m✗ 2FA password required\033[0m");
                        return false;
                    }
                    if (!send_password(pw_cb_())) {
                        return false;
                    }
                    break;
                case AuthState::WaitOtherDevice:
                    {
                        std::unique_lock lock(mutex_);
                        cv_.wait_for(lock, std::chrono::seconds(300), [this] {
                            return auth_state_.load() != AuthState::WaitOtherDevice;
                        });
                    }
                    break;
                case AuthState::Ready:
                    return true;
                case AuthState::Failed:
                    return false;
                default:
                    break;
            }
        }
    }

    void logout() {
        if (!client_ || !ready_) return;
        send_query_sync(make_object<tda::logOut>());
        {
            std::lock_guard lock(mutex_);
            auth_state_ = AuthState::None;
            ready_ = false;
        }
        cv_.notify_all();
    }

    int32_t api_id() const { return api_id_; }
    std::string api_hash() const { return api_hash_; }
    void set_api_params(int32_t id, const std::string& hash) {
        api_id_ = id;
        api_hash_ = hash;
    }

    // ── Channel operations ──────────────────────────────────────────
    bool create_channel(const std::string& title) {
        auto create = make_object<tda::createNewSupergroupChat>();
        create->title_ = title;
        create->is_channel_ = true;
        create->description_ = "TeleVault encrypted storage";

        auto result = send_query_sync(std::move(create));
        if (!result) return false;
        if (result->get_id() == tda::chat::ID) {
            channel_id_ = static_cast<tda::chat&>(*result).id_;
            return true;
        }
        if (result->get_id() == tda::error::ID) {
            auto& err = static_cast<tda::error&>(*result);
            spdlog::error("Failed to create channel: {} (code {})", err.message_, err.code_);
        }
        return false;
    }

    bool set_channel(int64_t channel_id) {
        channel_id_ = channel_id;
        auto open = make_object<tda::openChat>();
        open->chat_id_ = channel_id;
        send_query_sync(std::move(open));
        return true;
    }

    bool is_valid_channel(int64_t channel_id) const {
        auto get = make_object<tda::getChat>();
        get->chat_id_ = channel_id;
        auto result = const_cast<Impl*>(this)->send_query_sync(std::move(get));
        return result && result->get_id() == tda::chat::ID;
    }

    int64_t get_channel_id() const { return channel_id_; }

    // ── Message operations ──────────────────────────────────────────
    int64_t send_text(int64_t chat_id, const std::string& text, int64_t reply_to) {
        auto send = make_object<tda::sendMessage>();
        send->chat_id_ = chat_id;
        if (reply_to > 0) {
            send->reply_to_message_id_ = to_tdlib_msg_id(reply_to);
        }

        auto content = make_object<tda::inputMessageText>();
        auto formatted = make_object<tda::formattedText>();
        formatted->text_ = maybe_compress(text);
        content->text_ = std::move(formatted);
        send->input_message_content_ = std::move(content);

        auto result = send_query_sync(std::move(send));
        if (!result || result->get_id() != tda::message::ID) {
            if (result && result->get_id() == tda::error::ID) {
                auto& err = static_cast<tda::error&>(*result);
                spdlog::error("send_text failed: {} (code {})", err.message_, err.code_);
            }
            return 0;
        }
        auto& msg = static_cast<tda::message&>(*result);
        return wait_for_send(msg.id_, msg.sending_state_);
    }

    int64_t send_file(int64_t chat_id, const std::string& file_path, int64_t reply_to) {
        // 1. Upload file first
        auto upload = make_object<tda::uploadFile>();
        upload->file_ = make_object<tda::inputFileLocal>(file_path);
        upload->file_type_ = make_object<tda::fileTypeDocument>();

        // Wait for upload to complete and get file_id
        struct UploadState {
            int32_t file_id{};
            uint64_t size{};
            bool done{};
        };
        auto state = std::make_shared<UploadState>();

        upload->priority_ = 32;
        auto upload_result = send_query_sync(std::move(upload));

        if (!upload_result || upload_result->get_id() != tda::file::ID) {
            if (upload_result && upload_result->get_id() == tda::error::ID) {
                auto& err = static_cast<tda::error&>(*upload_result);
                spdlog::error("File upload failed: {} (code {})", err.message_, err.code_);
            }
            return 0;
        }

        auto& uploaded_file = static_cast<tda::file&>(*upload_result);

        // 2. Send as document message
        auto send = make_object<tda::sendMessage>();
        send->chat_id_ = chat_id;
        if (reply_to > 0) {
            send->reply_to_message_id_ = to_tdlib_msg_id(reply_to);
        }

        auto doc = make_object<tda::inputMessageDocument>();
        doc->document_ = make_object<tda::inputFileId>(uploaded_file.id_);
        send->input_message_content_ = std::move(doc);

        auto result = send_query_sync(std::move(send));
        if (!result || result->get_id() != tda::message::ID) {
            return 0;
        }
        auto& msg = static_cast<tda::message&>(*result);
        return wait_for_send(msg.id_, msg.sending_state_);
    }

    bool edit_message(int64_t chat_id, int64_t msg_id, const std::string& text) {
        auto edit = make_object<tda::editMessageText>();
        edit->chat_id_ = chat_id;
        edit->message_id_ = to_tdlib_msg_id(msg_id);

        auto content = make_object<tda::inputMessageText>();
        auto formatted = make_object<tda::formattedText>();
        formatted->text_ = maybe_compress(text);
        content->text_ = std::move(formatted);
        edit->input_message_content_ = std::move(content);

        auto result = send_query_sync(std::move(edit));
        if (!result || result->get_id() != tda::message::ID) {
            if (result && result->get_id() == tda::error::ID) {
                auto& err = static_cast<tda::error&>(*result);
                spdlog::warn("edit_message failed for msg {}: {} (code {})",
                             msg_id, err.message_, err.code_);
            }
            return false;
        }
        return true;
    }

    bool delete_messages(int64_t chat_id, const std::vector<int64_t>& msg_ids) {
        auto del = make_object<tda::deleteMessages>();
        del->chat_id_ = chat_id;
        for (auto id : msg_ids) {
            del->message_ids_.push_back(to_tdlib_msg_id(id));
        }
        del->revoke_ = true;

        auto result = send_query_sync(std::move(del));
        return result && result->get_id() == tda::ok::ID;
    }

    MessageInfo get_message(int64_t chat_id, int64_t raw_msg_id) const {
        int64_t msg_id = to_tdlib_msg_id(raw_msg_id);
        if (msg_id == 0) return {};

        auto get = make_object<tda::getMessage>();
        get->chat_id_ = chat_id;
        get->message_id_ = msg_id;

        auto result = const_cast<Impl*>(this)->send_query_sync(std::move(get));
        if (result && result->get_id() == tda::message::ID) {
            auto& msg = static_cast<tda::message&>(*result);
            MessageInfo info;
            info.id = msg.id_;
            info.text = extract_text(msg);
            info.file_id = extract_file_id(msg);
            info.reply_to_msg_id = msg.reply_to_message_id_;
            return info;
        }

        // TDLib getMessage only searches local DB. If not found locally, fetch from server via getMessages.
        auto get_many = make_object<tda::getMessages>();
        get_many->chat_id_ = chat_id;
        get_many->message_ids_ = {msg_id};

        auto many_result = const_cast<Impl*>(this)->send_query_sync(std::move(get_many));
        if (many_result && many_result->get_id() == tda::messages::ID) {
            auto& msgs = static_cast<tda::messages&>(*many_result);
            if (!msgs.messages_.empty() && msgs.messages_[0]) {
                auto& msg = *msgs.messages_[0];
                MessageInfo info;
                info.id = msg.id_;
                info.text = extract_text(msg);
                info.file_id = extract_file_id(msg);
                info.reply_to_msg_id = msg.reply_to_message_id_;
                return info;
            }
        } else if (many_result && many_result->get_id() == tda::error::ID) {
            auto& err = static_cast<tda::error&>(*many_result);
            spdlog::warn("getMessages failed for chat {} msg {}: {} (code {})",
                         chat_id, msg_id, err.message_, err.code_);
        }

        return {};
    }

    // Download a file by its message ID (extracts file_id from message)
    bool download_file_by_message(int64_t chat_id, int64_t msg_id, FileProgressCallback cb) {
        auto msg = get_message(chat_id, msg_id);
        if (msg.file_id == 0) {
            spdlog::error("Message {} has no file attachment", msg_id);
            return false;
        }
        return download_file(msg.file_id, std::move(cb));
    }

    // Send file and return both message_id and file_id
    // (Type is TelegramClient::FileSendResult, defined in client.hpp)

    TelegramClient::FileSendResult send_file_with_id(int64_t chat_id, const std::string& file_path, int64_t reply_to) {
        TelegramClient::FileSendResult result;

        auto upload = make_object<tda::uploadFile>();
        upload->file_ = make_object<tda::inputFileLocal>(file_path);
        upload->file_type_ = make_object<tda::fileTypeDocument>();
        upload->priority_ = 32;

        auto upload_result = send_query_sync(std::move(upload));
        if (!upload_result || upload_result->get_id() != tda::file::ID) {
            spdlog::error("File upload failed");
            return result;
        }

        auto& uploaded_file = static_cast<tda::file&>(*upload_result);
        result.file_id = uploaded_file.id_;

        auto send = make_object<tda::sendMessage>();
        send->chat_id_ = chat_id;
        if (reply_to > 0) {
            send->reply_to_message_id_ = to_tdlib_msg_id(reply_to);
        }

        auto doc = make_object<tda::inputMessageDocument>();
        doc->document_ = make_object<tda::inputFileId>(uploaded_file.id_);
        send->input_message_content_ = std::move(doc);

        auto send_result = send_query_sync(std::move(send));
        if (send_result && send_result->get_id() == tda::message::ID) {
            auto& msg = static_cast<tda::message&>(*send_result);
            result.message_id = wait_for_send(msg.id_, msg.sending_state_);
        }

        return result;
    }

    std::vector<MessageInfo> get_chat_history(int64_t chat_id, int64_t from_msg_id, int limit) const {
        std::vector<MessageInfo> out;
        int64_t current_from = to_tdlib_msg_id(from_msg_id);

        while (static_cast<int>(out.size()) < limit) {
            auto hist = make_object<tda::getChatHistory>();
            hist->chat_id_ = chat_id;
            hist->from_message_id_ = current_from;
            hist->offset_ = 0;
            hist->limit_ = std::min(100, limit - static_cast<int>(out.size()));
            hist->only_local_ = false;

            auto result = const_cast<Impl*>(this)->send_query_sync(std::move(hist));
            if (!result || result->get_id() != tda::messages::ID) {
                if (result && result->get_id() == tda::error::ID) {
                    auto& err = static_cast<tda::error&>(*result);
                    spdlog::warn("get_chat_history failed: {} (code {})", err.message_, err.code_);
                }
                break;
            }

            auto& msgs = static_cast<tda::messages&>(*result);
            if (msgs.messages_.empty()) break;

            int64_t last_id = current_from;
            for (auto& msg_ptr : msgs.messages_) {
                if (!msg_ptr) continue;
                last_id = msg_ptr->id_;
                MessageInfo info;
                info.id = msg_ptr->id_;
                info.text = extract_text(*msg_ptr);
                out.push_back(std::move(info));
            }

            if (last_id == current_from) break;
            current_from = last_id;
        }

        spdlog::debug("get_chat_history: total retrieved {} messages", out.size());
        return out;
    }

    int64_t get_pinned_message_id(int64_t chat_id) const {
        auto get = make_object<tda::getChat>();
        get->chat_id_ = chat_id;

        auto result = const_cast<Impl*>(this)->send_query_sync(std::move(get));
        if (!result || result->get_id() != tda::chat::ID) {
            spdlog::warn("get_pinned_message_id: getChat failed, id={}", result ? result->get_id() : -1);
            return 0;
        }
        // tdlib 1.8: use getChatPinnedMessage function
        auto pin_get = make_object<tda::getChatPinnedMessage>();
        pin_get->chat_id_ = chat_id;
        auto pin_result = const_cast<Impl*>(this)->send_query_sync(std::move(pin_get));
        if (!pin_result || pin_result->get_id() != tda::message::ID) {
            if (pin_result && pin_result->get_id() == tda::error::ID) {
                auto& err = static_cast<tda::error&>(*pin_result);
                spdlog::warn("get_pinned_message_id: getChatPinnedMessage error: {} (code {})", err.message_, err.code_);
            } else {
                spdlog::warn("get_pinned_message_id: pin_result id={}", pin_result ? pin_result->get_id() : -1);
            }
            return 0;
        }
        return to_tdlib_msg_id(static_cast<tda::message&>(*pin_result).id_);
    }

    bool pin_message(int64_t chat_id, int64_t msg_id) {
        auto pin = make_object<tda::pinChatMessage>();
        pin->chat_id_ = chat_id;
        pin->message_id_ = to_tdlib_msg_id(msg_id);
        pin->only_for_self_ = false;
        pin->disable_notification_ = true;

        auto result = send_query_sync(std::move(pin));
        if (!result || result->get_id() != tda::ok::ID) {
            if (result && result->get_id() == tda::error::ID) {
                auto& err = static_cast<tda::error&>(*result);
                spdlog::error("pin_message failed: {} (code {})", err.message_, err.code_);
            }
            return false;
        }
        return true;
    }

    // ── File operations ─────────────────────────────────────────────
    bool download_file(int32_t file_id, FileProgressCallback cb) const {
        auto self = const_cast<Impl*>(this);
        self->file_progress_cb_ = cb;

        // Fast path: check if file is already completely downloaded
        auto existing = get_file_info(file_id);
        if (existing && !existing->local_path.empty() && std::filesystem::exists(existing->local_path)) {
            if (existing->size > 0 && existing->downloaded_size >= existing->size) {
                return true;
            }
        }

        std::shared_ptr<std::promise<bool>> promise;
        std::future<bool> future;
        {
            std::lock_guard lock(self->download_mutex_);
            promise = std::make_shared<std::promise<bool>>();
            future = promise->get_future();
            self->pending_downloads_[file_id] = promise;
        }

        auto download = make_object<tda::downloadFile>();
        download->file_id_ = file_id;
        download->priority_ = 32;
        download->offset_ = 0;
        download->limit_ = 0;
        download->synchronous_ = false;

        auto result = self->send_query_sync(std::move(download));
        if (!result || result->get_id() != tda::file::ID) {
            std::lock_guard lock(self->download_mutex_);
            self->pending_downloads_.erase(file_id);
            return false;
        }

        auto& file = static_cast<tda::file&>(*result);
        if (file.local_->is_downloading_completed_) {
            std::lock_guard lock(self->download_mutex_);
            self->pending_downloads_.erase(file_id);
            return true;
        }

        // Wait for updateFile to signal download completion
        auto status = future.wait_for(std::chrono::seconds(180));
        if (status == std::future_status::ready) {
            return future.get();
        }

        std::lock_guard lock(self->download_mutex_);
        self->pending_downloads_.erase(file_id);
        spdlog::error("File download timed out for file_id {}", file_id);
        return false;
    }

    std::optional<FileInfo> get_file_info(int32_t file_id) const {
        auto get = make_object<tda::getFile>();
        get->file_id_ = file_id;

        auto result = const_cast<Impl*>(this)->send_query_sync(std::move(get));
        if (!result || result->get_id() != tda::file::ID) return std::nullopt;

        auto& file = static_cast<tda::file&>(*result);
        FileInfo info;
        info.file_id = file.id_;
        info.size = file.size_;
        info.downloaded_size = file.local_->downloaded_size_;
        if (file.local_->path_.size() > 0) {
            info.local_path = file.local_->path_;
        }
        return info;
    }

    // ── Account info ────────────────────────────────────────────────
    std::string get_my_username() const {
        auto me = make_object<tda::getMe>();
        auto result = const_cast<Impl*>(this)->send_query_sync(std::move(me));
        if (!result || result->get_id() != tda::user::ID) return {};
        return static_cast<tda::user&>(*result).username_;
    }

    int64_t get_my_id() const {
        auto me = make_object<tda::getMe>();
        auto result = const_cast<Impl*>(this)->send_query_sync(std::move(me));
        if (!result || result->get_id() != tda::user::ID) return 0;
        return static_cast<tda::user&>(*result).id_;
    }

private:
    // ── Tdlib types ─────────────────────────────────────────────────
    using ObjectPtr = tda::object_ptr<tda::Object>;
    using FunctionPtr = tda::object_ptr<tda::Function>;

    enum class AuthState {
        None,
        WaitPhone,
        WaitCode,
        WaitOtherDevice,
        WaitPassword,
        Ready,
        Failed
    };

    // ── Internal helpers ────────────────────────────────────────────

    // Request QR code authentication from Telegram
    bool request_qr_code() {
        auto req = make_object<tda::requestQrCodeAuthentication>();
        auto result = send_query_sync(std::move(req));
        if (!result || result->get_id() == tda::error::ID) {
            if (result) {
                auto& err = static_cast<tda::error&>(*result);
                spdlog::error("requestQrCodeAuthentication failed: {} (code {})", err.message_, err.code_);
            }
            return false;
        }
        return true;
    }

    // Send query asynchronously and get a future for the response
    std::future<ObjectPtr> send_query_async(FunctionPtr fn) {
        auto promise = std::make_shared<std::promise<ObjectPtr>>();
        auto future = promise->get_future();

        if (!client_) {
            spdlog::error("Telegram client is not connected");
            promise->set_value(nullptr);
            return future;
        }

        std::lock_guard lock(mutex_);
        auto id = ++next_query_id_;
        pending_[id] = promise;
        client_->send({id, std::move(fn)});

        return future;
    }

    // Send query synchronously (block until response)
    ObjectPtr send_query_sync(FunctionPtr fn) {
        auto future = send_query_async(std::move(fn));
        auto status = future.wait_for(std::chrono::seconds(120));
        if (status != std::future_status::ready) {
            spdlog::error("Tdlib query timed out after 120s");
            return nullptr;
        }
        try {
            return future.get();
        } catch (const std::exception& e) {
            spdlog::error("Tdlib query failed: {}", e.what());
            return nullptr;
        }
    }

    int64_t wait_for_send(int64_t temp_id, const tda::object_ptr<tda::MessageSendingState>& sending_state) {
        if (!sending_state) {
            return to_tdlib_msg_id(temp_id);
        }

        std::shared_ptr<std::promise<int64_t>> promise;
        std::future<int64_t> future;
        {
            std::lock_guard lock(send_mutex_);
            if (auto it = completed_sends_.find(temp_id); it != completed_sends_.end()) {
                int64_t real_id = it->second;
                completed_sends_.erase(it);
                return to_tdlib_msg_id(real_id);
            }
            promise = std::make_shared<std::promise<int64_t>>();
            future = promise->get_future();
            pending_sends_[temp_id] = promise;
        }

        auto status = future.wait_for(std::chrono::seconds(60));
        if (status == std::future_status::ready) {
            return to_tdlib_msg_id(future.get());
        }

        std::lock_guard lock(send_mutex_);
        pending_sends_.erase(temp_id);
        spdlog::warn("Message send confirmation timed out for temp id {}", temp_id);
        return to_tdlib_msg_id(temp_id);
    }

    // ── Client event loop ───────────────────────────────────────────
    void client_loop() {
        while (running_) {
            auto response = client_->receive(1.0);
            if (!response.object) continue;

            if (response.id == 0) {
                // Update (no request_id)
                handle_update(std::move(response.object));
            } else {
                // Response to a query
                handle_response(response.id, std::move(response.object));
            }
        }
    }

    void handle_update(ObjectPtr obj) {
        switch (obj->get_id()) {
            case tda::updateAuthorizationState::ID:
                handle_auth_state(
                    std::move(static_cast<tda::updateAuthorizationState&>(*obj).authorization_state_));
                break;

            case tda::updateFile::ID:
                handle_file_update(std::move(static_cast<tda::updateFile&>(*obj).file_));
                break;

            case tda::updateMessageSendSucceeded::ID: {
                auto& upd = static_cast<tda::updateMessageSendSucceeded&>(*obj);
                int64_t old_id = upd.old_message_id_;
                int64_t new_id = upd.message_ ? upd.message_->id_ : 0;
                std::lock_guard lock(send_mutex_);
                if (auto it = pending_sends_.find(old_id); it != pending_sends_.end()) {
                    it->second->set_value(new_id);
                    pending_sends_.erase(it);
                } else {
                    completed_sends_[old_id] = new_id;
                }
                break;
            }

            case tda::updateMessageSendFailed::ID: {
                auto& upd = static_cast<tda::updateMessageSendFailed&>(*obj);
                int64_t old_id = upd.old_message_id_;
                spdlog::error("Message send failed: {} (code {})", upd.error_message_, upd.error_code_);
                std::lock_guard lock(send_mutex_);
                if (auto it = pending_sends_.find(old_id); it != pending_sends_.end()) {
                    it->second->set_value(0);
                    pending_sends_.erase(it);
                } else {
                    completed_sends_[old_id] = 0;
                }
                break;
            }

            case tda::updateConnectionState::ID: {
                auto& state = static_cast<tda::updateConnectionState&>(*obj);
                if (state.state_->get_id() == tda::connectionStateReady::ID) {
                    spdlog::debug("Tdlib connection ready");
                }
                break;
            }

            default:
                break;
        }
    }

    void handle_response(uint64_t request_id, ObjectPtr obj) {
        std::lock_guard lock(mutex_);
        if (auto it = pending_.find(request_id); it != pending_.end()) {
            it->second->set_value(std::move(obj));
            pending_.erase(it);
        }
    }

    void handle_auth_state(ObjectPtr state) {
        if (!state) return;
        spdlog::debug("Auth state update: type_id={}", state->get_id());
        AuthState new_state;
        switch (state->get_id()) {
            case tda::authorizationStateWaitTdlibParameters::ID:
                spdlog::debug("Got WaitTdlibParameters, state unchanged");
                cv_.notify_all();
                return;

            case tda::authorizationStateWaitEncryptionKey::ID:
                spdlog::debug("Got WaitEncryptionKey, providing empty key");
                send_query_async(make_object<tda::checkDatabaseEncryptionKey>());
                return;

            case tda::authorizationStateWaitPhoneNumber::ID:
                spdlog::debug("Got WaitPhoneNumber");
                new_state = AuthState::WaitPhone;
                break;

            case tda::authorizationStateWaitCode::ID:
                spdlog::debug("Got WaitCode");
                new_state = AuthState::WaitCode;
                break;

            case tda::authorizationStateWaitOtherDeviceConfirmation::ID: {
                auto& wait_other = static_cast<tda::authorizationStateWaitOtherDeviceConfirmation&>(*state);
                std::string link = wait_other.link_;
                spdlog::info("Confirm login on another device or scan QR code: {}", link);
                // Native in-terminal QR rendering (no external `qrencode`
                // dependency); always falls back to the raw link.
                print_login_qr(link);
                new_state = AuthState::WaitOtherDevice;
                break;
            }

            case tda::authorizationStateWaitRegistration::ID: {
                spdlog::error("Telegram account not registered for this phone number");
                std::println("\033[31m✗ Account is not registered on Telegram\033[0m");
                new_state = AuthState::Failed;
                break;
            }

            case tda::authorizationStateWaitPassword::ID:
                spdlog::debug("Got WaitPassword");
                new_state = AuthState::WaitPassword;
                break;

            case tda::authorizationStateReady::ID:
                spdlog::info("Telegram authorization ready");
                new_state = AuthState::Ready;
                break;

            case tda::authorizationStateClosed::ID:
            case tda::authorizationStateLoggingOut::ID:
            case tda::authorizationStateClosing::ID:
                new_state = AuthState::None;
                ready_ = false;
                break;

            default:
                spdlog::warn("Unhandled auth state type_id={}", state->get_id());
                return;
        }
        {
            std::lock_guard lock(mutex_);
            auth_state_ = new_state;
        }
        cv_.notify_all();
    }

    bool send_phone() {
        auto& cfg = ConfigManager::instance();
        auto phone = cfg.get().telegram.phone;
        if (phone.empty()) {
            spdlog::error("No phone number configured");
            std::println("\033[31m✗ No phone number configured\033[0m");
            {
                std::lock_guard lock(mutex_);
                auth_state_ = AuthState::Failed;
            }
            cv_.notify_all();
            return false;
        }

        auto set_phone = make_object<tda::setAuthenticationPhoneNumber>();
        set_phone->phone_number_ = phone;
        set_phone->settings_ = make_object<tda::phoneNumberAuthenticationSettings>();

        {
            std::lock_guard lock(mutex_);
            auth_state_ = AuthState::None;
        }

        auto result = send_query_sync(std::move(set_phone));
        if (!result) {
            spdlog::error("Failed to send phone number: query timed out");
            std::println("\033[31m✗ Telegram query timed out\033[0m");
            {
                std::lock_guard lock(mutex_);
                auth_state_ = AuthState::Failed;
            }
            cv_.notify_all();
            return false;
        }

        if (result->get_id() == tda::error::ID) {
            auto& err = static_cast<tda::error&>(*result);
            spdlog::warn("setAuthenticationPhoneNumber failed: {} (code {})", err.message_, err.code_);
            if (err.message_ == "UPDATE_APP_TO_LOGIN") {
                std::println("\033[33mTelegram restricted SMS login on this API layer (UPDATE_APP_TO_LOGIN).\033[0m");
                std::println("\033[32mSwitching to instant QR Code authentication...\033[0m");
                if (request_qr_code()) {
                    return true;
                }
            }
            std::println("\033[31m✗ Telegram auth error: {} (code {})\033[0m", err.message_, err.code_);
            {
                std::lock_guard lock(mutex_);
                auth_state_ = AuthState::Failed;
            }
            cv_.notify_all();
            return false;
        }

        return true;
    }

    bool send_code(const std::string& code) {
        auto check = make_object<tda::checkAuthenticationCode>();
        check->code_ = code;

        {
            std::lock_guard lock(mutex_);
            auth_state_ = AuthState::None;
        }

        auto result = send_query_sync(std::move(check));
        if (!result) {
            spdlog::error("Failed to verify code: query timed out");
            std::println("\033[31m✗ Telegram query timed out\033[0m");
            {
                std::lock_guard lock(mutex_);
                auth_state_ = AuthState::Failed;
            }
            cv_.notify_all();
            return false;
        }

        if (result->get_id() == tda::error::ID) {
            auto& err = static_cast<tda::error&>(*result);
            spdlog::error("checkAuthenticationCode failed: {} (code {})", err.message_, err.code_);
            std::println("\033[31m✗ Invalid code or error: {} (code {})\033[0m", err.message_, err.code_);
            {
                std::lock_guard lock(mutex_);
                auth_state_ = AuthState::Failed;
            }
            cv_.notify_all();
            return false;
        }

        return true;
    }

    bool send_password(const std::string& password) {
        auto check = make_object<tda::checkAuthenticationPassword>();
        check->password_ = password;

        {
            std::lock_guard lock(mutex_);
            auth_state_ = AuthState::None;
        }

        auto result = send_query_sync(std::move(check));
        if (!result) {
            spdlog::error("Failed to verify 2FA password: query timed out");
            std::println("\033[31m✗ Telegram query timed out\033[0m");
            {
                std::lock_guard lock(mutex_);
                auth_state_ = AuthState::Failed;
            }
            cv_.notify_all();
            return false;
        }

        if (result->get_id() == tda::error::ID) {
            auto& err = static_cast<tda::error&>(*result);
            spdlog::error("checkAuthenticationPassword failed: {} (code {})", err.message_, err.code_);
            std::println("\033[31m✗ 2FA password error: {} (code {})\033[0m", err.message_, err.code_);
            {
                std::lock_guard lock(mutex_);
                auth_state_ = AuthState::Failed;
            }
            cv_.notify_all();
            return false;
        }

        return true;
    }

    // ── File updates ────────────────────────────────────────────────
    void handle_file_update(ObjectPtr file_obj) {
        if (!file_obj || file_obj->get_id() != tda::file::ID) return;
        auto& file = static_cast<tda::file&>(*file_obj);
        if (file_progress_cb_) {
            file_progress_cb_(file.id_, file.local_->downloaded_size_, file.size_);
        }
        if (file.local_->is_downloading_completed_) {
            std::shared_ptr<std::promise<bool>> p;
            {
                std::lock_guard lock(download_mutex_);
                if (auto it = pending_downloads_.find(file.id_); it != pending_downloads_.end()) {
                    p = it->second;
                    pending_downloads_.erase(it);
                }
            }
            if (p) {
                try {
                    p->set_value(true);
                } catch (...) {}
            }
        } else if (!file.local_->is_downloading_active_ && !file.local_->can_be_downloaded_) {
            std::shared_ptr<std::promise<bool>> p;
            {
                std::lock_guard lock(download_mutex_);
                if (auto it = pending_downloads_.find(file.id_); it != pending_downloads_.end()) {
                    p = it->second;
                    pending_downloads_.erase(it);
                }
            }
            if (p) {
                try {
                    p->set_value(false);
                } catch (...) {}
            }
        }
    }

    // ── State ───────────────────────────────────────────────────────
    std::unique_ptr<::td::Client> client_;
    std::thread client_thread_;
    std::atomic<bool> running_{false};
    bool ready_{};

    std::atomic<AuthState> auth_state_{AuthState::None};
    AuthCodeCallback code_cb_;
    AuthPasswordCallback pw_cb_;
    FileProgressCallback file_progress_cb_;

    int32_t api_id_{};
    std::string api_hash_;
    std::string db_dir_;
    std::string files_dir_;
    int64_t channel_id_{};

    // Request tracking
    uint64_t next_query_id_{};
    std::map<uint64_t, std::shared_ptr<std::promise<ObjectPtr>>> pending_;
    std::map<int64_t, std::shared_ptr<std::promise<int64_t>>> pending_sends_;
    std::map<int64_t, int64_t> completed_sends_;
    std::map<int32_t, std::shared_ptr<std::promise<bool>>> pending_downloads_;
    std::mutex mutex_;
    std::mutex send_mutex_;
    std::mutex download_mutex_;
    std::condition_variable cv_;
};

// ── Public API ────────────────────────────────────────────────────────
TelegramClient::TelegramClient() : impl_(std::make_unique<Impl>()) {}
TelegramClient::~TelegramClient() = default;

bool TelegramClient::connect() { return impl_->connect(); }
bool TelegramClient::is_authorized() const { return impl_->is_authorized(); }
bool TelegramClient::login(AuthCodeCallback code, AuthPasswordCallback pw) {
    return impl_->login(std::move(code), std::move(pw));
}
bool TelegramClient::login_qr(AuthPasswordCallback pw) {
    return impl_->login_qr(std::move(pw));
}
void TelegramClient::logout() { impl_->logout(); }
int32_t TelegramClient::api_id() const { return impl_->api_id(); }
std::string TelegramClient::api_hash() const { return impl_->api_hash(); }
void TelegramClient::set_api_params(int32_t id, const std::string& hash) {
    impl_->set_api_params(id, hash);
}

bool TelegramClient::create_channel(const std::string& title) { return impl_->create_channel(title); }
bool TelegramClient::set_channel(int64_t id) { return impl_->set_channel(id); }
bool TelegramClient::is_valid_channel(int64_t id) const { return impl_->is_valid_channel(id); }
int64_t TelegramClient::get_channel_id() const { return impl_->get_channel_id(); }

int64_t TelegramClient::send_text(int64_t chat, const std::string& text, int64_t reply) {
    return impl_->send_text(chat, text, reply);
}
int64_t TelegramClient::send_file(int64_t chat, const std::string& path, int64_t reply) {
    return impl_->send_file(chat, path, reply);
}
bool TelegramClient::edit_message(int64_t chat, int64_t msg, const std::string& text) {
    return impl_->edit_message(chat, msg, text);
}
bool TelegramClient::delete_messages(int64_t chat, const std::vector<int64_t>& ids) {
    return impl_->delete_messages(chat, ids);
}
MessageInfo TelegramClient::get_message(int64_t chat, int64_t msg) const {
    return impl_->get_message(chat, msg);
}
std::vector<MessageInfo> TelegramClient::get_chat_history(int64_t chat, int64_t from, int limit) const {
    return impl_->get_chat_history(chat, from, limit);
}
int64_t TelegramClient::get_pinned_message_id(int64_t chat) const {
    return impl_->get_pinned_message_id(chat);
}
bool TelegramClient::pin_message(int64_t chat, int64_t msg) {
    return impl_->pin_message(chat, msg);
}

bool TelegramClient::download_file(int32_t file_id, FileProgressCallback cb) const {
    return impl_->download_file(file_id, std::move(cb));
}
bool TelegramClient::download_file_by_message(int64_t chat_id, int64_t msg_id, FileProgressCallback cb) const {
    return impl_->download_file_by_message(chat_id, msg_id, std::move(cb));
}
std::optional<FileInfo> TelegramClient::get_file_info(int32_t file_id) const {
    return impl_->get_file_info(file_id);
}
TelegramClient::FileSendResult TelegramClient::send_file_with_id(int64_t chat_id, const std::string& path, int64_t reply_to) {
    return impl_->send_file_with_id(chat_id, path, reply_to);
}

std::string TelegramClient::get_my_username() const { return impl_->get_my_username(); }
int64_t TelegramClient::get_my_id() const { return impl_->get_my_id(); }

} // namespace tv
