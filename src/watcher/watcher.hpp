#pragma once

#include <string>
#include <memory>
#include <functional>
#include <vector>
#include <chrono>

namespace tv {

struct WatchedFile {
    std::string path;
    std::string hash;
    std::chrono::system_clock::time_point modified_at;
};

class FileWatcher {
public:
    explicit FileWatcher(std::string directory);
    ~FileWatcher();

    using ChangeCallback = std::function<void(const std::vector<std::string>&)>;

    void start(ChangeCallback cb);
    void stop();
    void set_exclusions(const std::vector<std::string>& patterns);
    void save_state() const;
    void load_state();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv
