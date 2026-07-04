#include "index.hpp"
#include "../telegram/client.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace tv {

IndexManager::IndexManager(TelegramClient& tg) : tg_(tg) {}

int64_t IndexManager::find_pinned_message_id(int64_t chat_id) const {
    return tg_.get_pinned_message_id(chat_id);
}

std::string IndexManager::serialize() const {
    nlohmann::json j = index_;
    return j.dump();
}

bool IndexManager::deserialize(const std::string& json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);
        index_ = j.get<VaultIndex>();
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Failed to parse vault index: {}", e.what());
        return false;
    }
}

bool IndexManager::load(int64_t chat_id) {
    auto pinned_id = find_pinned_message_id(chat_id);
    if (pinned_id == 0) {
        spdlog::warn("No pinned message found — starting with empty index");
        index_ = VaultIndex{};
        return true;
    }

    auto msg = tg_.get_message(chat_id, pinned_id);
    if (msg.text.empty()) {
        spdlog::warn("Pinned message is empty — starting with empty index");
        index_ = VaultIndex{};
        return true;
    }

    return deserialize(msg.text);
}

bool IndexManager::save(int64_t chat_id) {
    auto json_str = serialize();

    auto pinned_id = find_pinned_message_id(chat_id);
    if (pinned_id == 0) {
        // No pinned message yet — send new one and pin it
        auto new_id = tg_.send_text(chat_id, json_str);
        if (new_id == 0) {
            spdlog::error("Failed to send index message");
            return false;
        }
        if (!tg_.pin_message(chat_id, new_id)) {
            spdlog::error("Failed to pin index message");
            return false;
        }
    } else {
        // Update existing pinned message
        if (!tg_.edit_message(chat_id, pinned_id, json_str)) {
            spdlog::error("Failed to update index message");
            return false;
        }
    }

    return true;
}

void IndexManager::add_file(const std::string& file_id, int64_t metadata_msg_id) {
    std::lock_guard lock(mutex_);
    index_.files[file_id] = metadata_msg_id;
}

void IndexManager::remove_file(const std::string& file_id) {
    std::lock_guard lock(mutex_);
    index_.files.erase(file_id);
}

std::optional<int64_t> IndexManager::get_metadata_msg_id(const std::string& file_id) const {
    std::lock_guard lock(mutex_);
    if (auto it = index_.files.find(file_id); it != index_.files.end()) {
        return it->second;
    }
    return std::nullopt;
}

} // namespace tv
