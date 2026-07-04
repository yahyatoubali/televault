#include "watcher.hpp"
#include "../chunker/hash.hpp"
#include "../util/config.hpp"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>

namespace tv {

class FileWatcher::Impl {
public:
    std::string directory_;
    mutable std::mutex exclusions_mutex_;
    std::vector<std::string> exclusions_{".git", "__pycache__", "node_modules"};
    std::thread poll_thread_;
    std::atomic<bool> running_{false};
    ChangeCallback callback_;
    std::unordered_map<std::string, WatchedFile> state_;

    bool is_excluded(const std::string& path) const {
        std::lock_guard<std::mutex> lock(exclusions_mutex_);
        for (auto& pat : exclusions_) {
            if (path.find(pat) != std::string::npos) return true;
        }
        return false;
    }

    void scan() {
        std::unordered_map<std::string, WatchedFile> current;

        for (auto& entry : std::filesystem::recursive_directory_iterator(
                 directory_, std::filesystem::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            auto path = entry.path().string();
            if (is_excluded(path)) continue;

            WatchedFile wf;
            wf.path = path;
            wf.modified_at = std::chrono::file_clock::to_sys(
                std::filesystem::last_write_time(entry));
            current[path] = wf;
        }

        // Detect changes
        std::vector<std::string> changed;
        for (auto& [path, wf] : current) {
            auto it = state_.find(path);
            if (it == state_.end() || it->second.modified_at != wf.modified_at) {
                // New or modified file
                try {
                    wf.hash = hash_file(path);
                } catch (...) {
                    continue;
                }
                changed.push_back(path);
                current[path] = wf;
            } else {
                current[path].hash = it->second.hash;
            }
        }

        // Detect deletions
        for (auto& [path, wf] : state_) {
            if (!current.contains(path)) {
                changed.push_back("[DELETED] " + path);
            }
        }

        state_ = std::move(current);

        if (!changed.empty() && callback_) {
            callback_(changed);
        }
    }

    void poll_loop() {
        while (running_) {
            scan();
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
};

FileWatcher::FileWatcher(std::string directory)
    : impl_(std::make_unique<Impl>()) {
    impl_->directory_ = std::move(directory);
}

FileWatcher::~FileWatcher() { stop(); }

void FileWatcher::start(ChangeCallback cb) {
    impl_->callback_ = std::move(cb);
    load_state();
    impl_->running_ = true;
    impl_->poll_thread_ = std::thread([this] { impl_->poll_loop(); });
    spdlog::info("Watching directory: {}", impl_->directory_);
}

void FileWatcher::stop() {
    impl_->running_ = false;
    if (impl_->poll_thread_.joinable()) {
        impl_->poll_thread_.join();
    }
    save_state();
}

void FileWatcher::set_exclusions(const std::vector<std::string>& patterns) {
    std::lock_guard lock(impl_->exclusions_mutex_);
    impl_->exclusions_ = patterns;
}

void FileWatcher::save_state() const {
    auto path = ConfigManager::instance().data_dir() + "/watcher/state.json";
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());

    nlohmann::json j = nlohmann::json::object();
    for (auto& [p, wf] : impl_->state_) {
        j[p] = {
            {"hash", wf.hash},
            {"modified_at", std::chrono::duration_cast<std::chrono::seconds>(
                                wf.modified_at.time_since_epoch()).count()}
        };
    }

    std::ofstream f(path);
    f << j.dump(2);
}

void FileWatcher::load_state() {
    auto path = ConfigManager::instance().data_dir() + "/watcher/state.json";
    std::ifstream f(path);
    if (!f.is_open()) return;

    try {
        auto j = nlohmann::json::parse(f);
        for (auto& [path, info] : j.items()) {
            WatchedFile wf;
            wf.path = path;
            wf.hash = info["hash"];
            int64_t ts = info["modified_at"];
            wf.modified_at = std::chrono::system_clock::from_time_t(
                static_cast<time_t>(ts));
            impl_->state_[path] = std::move(wf);
        }
    } catch (...) {}
}

} // namespace tv
