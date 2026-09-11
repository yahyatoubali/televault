#include "index.hpp"
#include "../telegram/client.hpp"
#include "../util/config.hpp"
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
        if (j.value("type", "") == "vault_index" || (j.contains("files") && j.contains("version"))) {
            index_ = j.get<VaultIndex>();
            return true;
        }
        if (j.contains("files") && j["files"].is_object()) {
            index_.files = j["files"].get<std::unordered_map<std::string, int64_t>>();
            index_.version = j.value("version", 1);
            return true;
        }
        return false;
    } catch (const std::exception& e) {
        spdlog::debug("IndexManager::deserialize: not a valid vault index: {}", e.what());
        return false;
    }
}

bool IndexManager::sync_internal(int64_t chat_id) {
    auto recent = tg_.get_chat_history(chat_id, 0, 50);
    if (recent.empty()) return false;

    bool updated = false;

    // 1. Scan recent messages for an updated vault_index message
    for (const auto& m : recent) {
        if (m.text.empty()) continue;
        try {
            auto j = nlohmann::json::parse(m.text);
            if (j.value("type", "") == "vault_index" || (j.contains("files") && j.contains("version"))) {
                auto remote_idx = j.get<VaultIndex>();
                for (const auto& [fid, mid] : remote_idx.files) {
                    if (auto it = index_.files.find(fid); it == index_.files.end()) {
                        index_.files[fid] = mid;
                        updated = true;
                    } else if (mid != it->second) {
                        it->second = mid;
                        updated = true;
                    }
                }
                if (index_message_id_ == 0 || m.id > index_message_id_) {
                    index_message_id_ = m.id;
                }
                break;
            }
        } catch (...) {}
    }

    // 2. Also scan recent messages for newly uploaded FileMetadata
    for (const auto& m : recent) {
        if (m.text.empty()) continue;
        try {
            auto j = nlohmann::json::parse(m.text);
            if (j.contains("id") && j.contains("name") && (j.contains("chunks") || j.contains("has_manifest"))) {
                std::string fid = j["id"].get<std::string>();
                if (auto it = index_.files.find(fid); it == index_.files.end()) {
                    index_.files[fid] = m.id;
                    updated = true;
                    spdlog::info("sync: discovered file fid={} name='{}' mid={} from channel",
                                 fid, j.value("name", ""), m.id);
                }
            }
        } catch (...) {}
    }

    return updated;
}

bool IndexManager::sync(int64_t chat_id) {
    std::lock_guard lock(mutex_);
    return sync_internal(chat_id);
}

bool IndexManager::load(int64_t chat_id) {
    std::lock_guard lock(mutex_);
    index_ = VaultIndex{};
    index_message_id_ = 0;

    // 1. Try finding pinned message
    auto pinned_id = find_pinned_message_id(chat_id);
    if (pinned_id != 0) {
        auto msg = tg_.get_message(chat_id, pinned_id);
        if (!msg.text.empty() && deserialize(msg.text)) {
            index_message_id_ = pinned_id;
            spdlog::debug("IndexManager::load: loaded index from pinned msg {}", pinned_id);
        }
    }

    // 2. Try configured index_msg_id if pinned message not found or empty
    if (index_message_id_ == 0 || index_.files.empty()) {
        auto cfg_mid = ConfigManager::instance().get().index_msg_id;
        if (cfg_mid != 0 && cfg_mid != pinned_id) {
            auto msg = tg_.get_message(chat_id, cfg_mid);
            if (!msg.text.empty() && deserialize(msg.text)) {
                index_message_id_ = cfg_mid;
                spdlog::debug("IndexManager::load: loaded index from config msg {}", cfg_mid);
            }
        }
    }

    // 3. Scan recent channel history to merge latest index and any newly pushed files
    sync_internal(chat_id);

    // 4. If index is still empty, scan full channel history (disaster recovery / fresh machine auto-discovery)
    if (index_.files.empty()) {
        spdlog::info("IndexManager::load: index empty, scanning channel history to auto-discover files...");
        int64_t current_from = 0;
        int recovered = 0;
        while (true) {
            auto history = tg_.get_chat_history(chat_id, current_from, 100);
            if (history.empty()) break;
            int64_t last_id = current_from;
            for (const auto& msg : history) {
                last_id = msg.id;
                if (msg.text.empty()) continue;
                try {
                    auto j = nlohmann::json::parse(msg.text);
                    if (index_message_id_ == 0 && (j.value("type", "") == "vault_index" || (j.contains("files") && j.contains("version")))) {
                        if (deserialize(msg.text)) {
                            index_message_id_ = msg.id;
                        }
                    } else if (j.contains("id") && j.contains("name") && (j.contains("chunks") || j.contains("has_manifest"))) {
                        std::string fid = j["id"].get<std::string>();
                        if (!index_.files.contains(fid)) {
                            index_.files[fid] = msg.id;
                            recovered++;
                        }
                    }
                } catch (...) {}
            }
            if (last_id == current_from || history.size() < 100) break;
            current_from = last_id;
        }
        if (recovered > 0) {
            spdlog::info("IndexManager::load: auto-discovered {} files across channel history", recovered);
        }
    }

    // Attempt to pin if we found an index that isn't pinned
    if (index_message_id_ != 0 && pinned_id == 0) {
        tg_.pin_message(chat_id, index_message_id_);
    }

    // Save to local config if valid
    if (index_message_id_ != 0) {
        auto cfg_copy = ConfigManager::instance().get();
        if (cfg_copy.index_msg_id != index_message_id_) {
            cfg_copy.index_msg_id = index_message_id_;
            ConfigManager::instance().set(cfg_copy);
            ConfigManager::instance().save();
        }
    }

    return true;
}

bool IndexManager::save(int64_t chat_id) {
    std::lock_guard lock(mutex_);
    // Sync before saving so we never wipe out files uploaded by other machines
    sync_internal(chat_id);

    index_.version++;
    auto json_str = serialize();
    spdlog::debug("IndexManager::save: serializing {} files (json len={})", index_.files.size(), json_str.size());

    bool saved = false;
    // 1. Try editing existing index_message_id_
    if (index_message_id_ != 0) {
        saved = tg_.edit_message(chat_id, index_message_id_, json_str);
    }

    // 2. Try editing pinned message if different
    if (!saved) {
        auto pinned_id = find_pinned_message_id(chat_id);
        if (pinned_id != 0 && pinned_id != index_message_id_) {
            saved = tg_.edit_message(chat_id, pinned_id, json_str);
            if (saved) {
                index_message_id_ = pinned_id;
            }
        }
    }

    // 3. Send new message and pin it
    if (!saved) {
        auto new_id = tg_.send_text(chat_id, json_str);
        if (new_id == 0) {
            spdlog::error("Failed to send index message");
            return false;
        }
        index_message_id_ = new_id;
        tg_.pin_message(chat_id, new_id);
    }

    // Update config with active index message ID
    if (index_message_id_ != 0) {
        auto cfg_copy = ConfigManager::instance().get();
        if (cfg_copy.index_msg_id != index_message_id_) {
            cfg_copy.index_msg_id = index_message_id_;
            ConfigManager::instance().set(cfg_copy);
            ConfigManager::instance().save();
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
