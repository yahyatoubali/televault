#ifndef TV_BUILD_TDLIB

#include "telegram/client.hpp"
#include "telegram/auth.hpp"
#include "telegram/session.hpp"
#include "core/app_context.hpp"
#include "core/vault.hpp"

#include <stdexcept>

namespace tv {

// Ensure Impl types are complete for unique_ptr
struct TelegramClient::Impl {};
struct TeleVault::Impl {};

// ── TelegramClient stubs ──────────────────────────────────────────────

TelegramClient::TelegramClient() = default;
TelegramClient::~TelegramClient() = default;

bool TelegramClient::connect() { return false; }
bool TelegramClient::is_authorized() const { return false; }
bool TelegramClient::login(AuthCodeCallback, AuthPasswordCallback) { return false; }
void TelegramClient::logout() {}
int32_t TelegramClient::api_id() const { return 0; }
std::string TelegramClient::api_hash() const { return {}; }

bool TelegramClient::create_channel(const std::string&) { return false; }
bool TelegramClient::set_channel(int64_t) { return false; }
bool TelegramClient::is_valid_channel(int64_t) const { return false; }
int64_t TelegramClient::get_channel_id() const { return 0; }

int64_t TelegramClient::send_text(int64_t, const std::string&, int64_t) { return 0; }
int64_t TelegramClient::send_file(int64_t, const std::string&, int64_t) { return 0; }
bool TelegramClient::edit_message(int64_t, int64_t, const std::string&) { return false; }
bool TelegramClient::delete_messages(int64_t, const std::vector<int64_t>&) { return false; }
MessageInfo TelegramClient::get_message(int64_t, int64_t) const { return {}; }
std::vector<MessageInfo> TelegramClient::get_chat_history(int64_t, int64_t, int) const { return {}; }
int64_t TelegramClient::get_pinned_message_id(int64_t) const { return 0; }
bool TelegramClient::pin_message(int64_t, int64_t) { return false; }

bool TelegramClient::download_file(int32_t, FileProgressCallback) const { return false; }
bool TelegramClient::download_file_by_message(int64_t, int64_t, FileProgressCallback) const { return false; }
std::optional<FileInfo> TelegramClient::get_file_info(int32_t) const { return std::nullopt; }
TelegramClient::FileSendResult TelegramClient::send_file_with_id(int64_t, const std::string&, int64_t) { return {}; }

std::string TelegramClient::get_my_username() const { return {}; }
int64_t TelegramClient::get_my_id() const { return 0; }
void TelegramClient::set_api_params(int32_t, const std::string&) {}

// ── AuthFlow stubs ────────────────────────────────────────────────────

AuthFlow::AuthFlow(TelegramClient& client) : client_(client) {}
AuthFlow::State AuthFlow::execute(const std::string&, CodeCallback, PasswordCallback) { return State::Failed; }
void AuthFlow::logout() {}

// ── SessionManager stubs ──────────────────────────────────────────────

SessionManager::SessionManager() = default;
std::string SessionManager::db_dir() const { return {}; }
std::string SessionManager::files_dir() const { return {}; }
bool SessionManager::has_api_credentials() const { return false; }
bool SessionManager::save_api_credentials(int32_t, const std::string&, const std::string&) { return false; }
void SessionManager::clear() {}
std::string SessionManager::resolve_data_dir() { return {}; }

// ── AppContext stubs ──────────────────────────────────────────────────

bool AppContext::initialize() { return false; }
bool AppContext::ensure_vault() { return false; }
void AppContext::shutdown() {}

// ── TeleVault stubs ───────────────────────────────────────────────────

TeleVault::TeleVault(TelegramClient&) : impl_(std::make_unique<Impl>()) {}
TeleVault::~TeleVault() = default;
bool TeleVault::initialize(int64_t, bool) { return false; }
bool TeleVault::push(const std::string&, const VaultOptions&, ProgressCallback) { return false; }
bool TeleVault::pull(const std::string&, const std::string&, const VaultOptions&, ProgressCallback) { return false; }
bool TeleVault::cat(const std::string&, const VaultOptions&, ProgressCallback) { return false; }
std::vector<FileEntry> TeleVault::list_files() const { return {}; }
std::vector<FileEntry> TeleVault::list_trash() const { return {}; }
std::vector<FileEntry> TeleVault::find_files(const std::string&) const { return {}; }
std::vector<FileMetadata> TeleVault::find_all_matching(const std::string&) const { return {}; }
std::optional<std::vector<uint8_t>> TeleVault::read_byte_range(const std::string&, uint64_t, uint64_t, const VaultOptions&) { return std::nullopt; }
std::optional<FileMetadata> TeleVault::get_file_info(const std::string&) const { return std::nullopt; }
bool TeleVault::delete_file(const std::string&, bool) { return false; }
bool TeleVault::restore_file(const std::string&) { return false; }
bool TeleVault::empty_trash() { return false; }
void TeleVault::set_shards(std::vector<int64_t>) {}
bool TeleVault::verify_file(const std::string&) { return false; }

} // namespace tv

#endif
