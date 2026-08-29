#include "client.hpp"
#include "session.hpp"
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

namespace tv {

namespace tda = ::td::td_api; // alias for shorter names
using tda::make_object;

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

// ── Helper: extract text from message ─────────────────────────────────
static std::string extract_text(const tda::message& msg) {
    if (msg.content_->get_id() == tda::messageText::ID) {
        return static_cast<const tda::messageText&>(*msg.content_).text_->text_;
    }
    return {};
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

        // Send parameters asynchronously
        auto result_future = send_query_async(std::move(params));

        // Wait for client ready
        {
            std::unique_lock lock(mutex_);
            if (!cv_.wait_for(lock, std::chrono::seconds(60), [this] {
                return auth_state_ >= AuthState::WaitPhone;
            })) {
                // Check if setTdlibParameters returned an error
                if (result_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    auto result = result_future.get();
                    if (result && result->get_id() == tda::error::ID) {
                        auto& err = static_cast<tda::error&>(*result);
                        spdlog::error("setTdlibParameters failed: {} (code {})", err.message_, err.code_);
                    } else {
                        spdlog::error("setTdlibParameters returned unexpected result");
                    }
                } else {
                    spdlog::error("Timed out waiting for Tdlib client to initialize (auth_state={})",
                        static_cast<int>(auth_state_.load()));
                }
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
    }

    bool is_authorized() const {
        return auth_state_.load(std::memory_order_acquire) == AuthState::Ready;
    }

    // ── Authentication ──────────────────────────────────────────────
    bool login(AuthCodeCallback code_cb, AuthPasswordCallback pw_cb) {
        if (!ready_ && !connect()) return false;

        code_cb_ = std::move(code_cb);
        pw_cb_ = std::move(pw_cb);

        while (auth_state_ < AuthState::Ready) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            switch (auth_state_) {
                case AuthState::WaitPhone:
                    send_phone();
                    break;
                case AuthState::WaitCode:
                    if (!code_cb_) break;
                    send_code(code_cb_());
                    break;
                case AuthState::WaitPassword:
                    if (!pw_cb_) {
                        spdlog::error("2FA password required but no callback provided");
                        return false;
                    }
                    send_password(pw_cb_());
                    break;
                case AuthState::Ready:
                    return true;
                case AuthState::Failed:
                    return false;
                default:
                    break;
            }
        }
        return auth_state_ == AuthState::Ready;
    }

    void logout() {
        if (!client_ || !ready_) return;
        send_query_async(make_object<tda::logOut>());
        auth_state_ = AuthState::None;
        ready_ = false;
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
            send->reply_to_message_id_ = reply_to;
        }

        auto content = make_object<tda::inputMessageText>();
        auto formatted = make_object<tda::formattedText>();
        formatted->text_ = text;
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
        return static_cast<tda::message&>(*result).id_;
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
            send->reply_to_message_id_ = reply_to;
        }

        auto doc = make_object<tda::inputMessageDocument>();
        doc->document_ = make_object<tda::inputFileId>(uploaded_file.id_);
        send->input_message_content_ = std::move(doc);

        auto result = send_query_sync(std::move(send));
        if (!result || result->get_id() != tda::message::ID) {
            return 0;
        }
        return static_cast<tda::message&>(*result).id_;
    }

    bool edit_message(int64_t chat_id, int64_t msg_id, const std::string& text) {
        auto edit = make_object<tda::editMessageText>();
        edit->chat_id_ = chat_id;
        edit->message_id_ = msg_id;

        auto content = make_object<tda::inputMessageText>();
        auto formatted = make_object<tda::formattedText>();
        formatted->text_ = text;
        content->text_ = std::move(formatted);
        edit->input_message_content_ = std::move(content);

        auto result = send_query_sync(std::move(edit));
        return result && result->get_id() == tda::message::ID;
    }

    bool delete_messages(int64_t chat_id, const std::vector<int64_t>& msg_ids) {
        auto del = make_object<tda::deleteMessages>();
        del->chat_id_ = chat_id;
        del->message_ids_ = msg_ids;
        del->revoke_ = true;

        auto result = send_query_sync(std::move(del));
        return result && result->get_id() == tda::ok::ID;
    }

    MessageInfo get_message(int64_t chat_id, int64_t msg_id) const {
        auto get = make_object<tda::getMessage>();
        get->chat_id_ = chat_id;
        get->message_id_ = msg_id;

        auto result = const_cast<Impl*>(this)->send_query_sync(std::move(get));
        if (!result || result->get_id() != tda::message::ID) {
            return {};
        }

        auto& msg = static_cast<tda::message&>(*result);
        MessageInfo info;
        info.id = msg.id_;
        info.text = extract_text(msg);
        info.file_id = extract_file_id(msg);
        info.reply_to_msg_id = msg.reply_to_message_id_;
        return info;
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
            send->reply_to_message_id_ = reply_to;
        }

        auto doc = make_object<tda::inputMessageDocument>();
        doc->document_ = make_object<tda::inputFileId>(uploaded_file.id_);
        send->input_message_content_ = std::move(doc);

        auto send_result = send_query_sync(std::move(send));
        if (send_result && send_result->get_id() == tda::message::ID) {
            result.message_id = static_cast<tda::message&>(*send_result).id_;
        }

        return result;
    }

    std::vector<MessageInfo> get_chat_history(int64_t chat_id, int64_t from_msg_id, int limit) const {
        auto hist = make_object<tda::getChatHistory>();
        hist->chat_id_ = chat_id;
        hist->from_message_id_ = from_msg_id;
        hist->offset_ = 0;
        hist->limit_ = limit;
        hist->only_local_ = false;

        auto result = const_cast<Impl*>(this)->send_query_sync(std::move(hist));
        if (!result || result->get_id() != tda::messages::ID) {
            return {};
        }

        auto& msgs = static_cast<tda::messages&>(*result);
        std::vector<MessageInfo> out;
        out.reserve(msgs.messages_.size());

        for (auto& msg_ptr : msgs.messages_) {
            if (!msg_ptr) continue;
            MessageInfo info;
            info.id = msg_ptr->id_;
            info.text = extract_text(*msg_ptr);
            out.push_back(std::move(info));
        }
        return out;
    }

    int64_t get_pinned_message_id(int64_t chat_id) const {
        auto get = make_object<tda::getChat>();
        get->chat_id_ = chat_id;

        auto result = const_cast<Impl*>(this)->send_query_sync(std::move(get));
        if (!result || result->get_id() != tda::chat::ID) return 0;
        // tdlib 1.8: use getChatPinnedMessage function
        auto pin_get = make_object<tda::getChatPinnedMessage>();
        pin_get->chat_id_ = chat_id;
        auto pin_result = const_cast<Impl*>(this)->send_query_sync(std::move(pin_get));
        if (!pin_result || pin_result->get_id() != tda::message::ID) return 0;
        return static_cast<tda::message&>(*pin_result).id_;
    }

    bool pin_message(int64_t chat_id, int64_t msg_id) {
        auto pin = make_object<tda::pinChatMessage>();
        pin->chat_id_ = chat_id;
        pin->message_id_ = msg_id;
        pin->only_for_self_ = false;
        pin->disable_notification_ = true;

        auto result = send_query_sync(std::move(pin));
        return result && result->get_id() == tda::ok::ID;
    }

    // ── File operations ─────────────────────────────────────────────
    bool download_file(int32_t file_id, FileProgressCallback cb) const {
        // Store the progress callback for handle_file_update in the client thread
        const_cast<Impl*>(this)->file_progress_cb_ = cb;

        auto download = make_object<tda::downloadFile>();
        download->file_id_ = file_id;
        download->priority_ = 32;
        download->offset_ = 0;
        download->limit_ = 0;
        download->synchronous_ = false;

        auto result = const_cast<Impl*>(this)->send_query_sync(std::move(download));
        if (!result || result->get_id() != tda::file::ID) return false;

        auto& file = static_cast<tda::file&>(*result);
        if (file.local_->is_downloading_completed_) return true;

        // If synchronous mode fails to complete, try with progress
        spdlog::warn("File download may be incomplete: {}/{}",
                     file.local_->downloaded_size_, file.size_);
        return file.local_->is_downloading_completed_;
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
        WaitPassword,
        Ready,
        Failed
    };

    // ── Internal helpers ────────────────────────────────────────────

    // Send query asynchronously and get a future for the response
    std::future<ObjectPtr> send_query_async(FunctionPtr fn) {
        auto promise = std::make_shared<std::promise<ObjectPtr>>();
        auto future = promise->get_future();

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
                new_state = AuthState::WaitPhone;
                break;

            case tda::authorizationStateWaitCode::ID:
                new_state = AuthState::WaitCode;
                break;

            case tda::authorizationStateWaitPassword::ID:
                new_state = AuthState::WaitPassword;
                break;

            case tda::authorizationStateReady::ID:
                new_state = AuthState::Ready;
                break;

            case tda::authorizationStateClosed::ID:
            case tda::authorizationStateLoggingOut::ID:
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

    void send_phone() {
        auto& cfg = ConfigManager::instance();
        auto phone = cfg.get().telegram.phone;
        if (phone.empty()) {
            spdlog::error("No phone number configured");
            auth_state_ = AuthState::Failed;
            return;
        }

        auto set_phone = make_object<tda::setAuthenticationPhoneNumber>();
        set_phone->phone_number_ = phone;
        auth_state_.store(AuthState::None, std::memory_order_release);
        send_query_async(std::move(set_phone));
    }

    void send_code(const std::string& code) {
        auto check = make_object<tda::checkAuthenticationCode>();
        check->code_ = code;
        send_query_async(std::move(check));
    }

    void send_password(const std::string& password) {
        auto check = make_object<tda::checkAuthenticationPassword>();
        check->password_ = password;
        send_query_async(std::move(check));
    }

    // ── File updates ────────────────────────────────────────────────
    void handle_file_update(ObjectPtr file_obj) {
        if (!file_obj || file_obj->get_id() != tda::file::ID) return;
        auto& file = static_cast<tda::file&>(*file_obj);
        if (file_progress_cb_) {
            file_progress_cb_(file.id_, file.local_->downloaded_size_, file.size_);
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
    std::mutex mutex_;
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
