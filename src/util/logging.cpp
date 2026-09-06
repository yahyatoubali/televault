#include "logging.hpp"
#include <filesystem>

namespace tv {

std::shared_ptr<spdlog::logger> Logging::root_logger_;

void Logging::setup(const std::string& log_dir, const std::string& level) {
    std::filesystem::create_directories(log_dir);

    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        log_dir + "/televault.log", 10 * 1024 * 1024, 3);

    std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
    root_logger_ = std::make_shared<spdlog::logger>("televault", sinks.begin(), sinks.end());
    set_level(level);
    spdlog::set_default_logger(root_logger_);
}

void Logging::set_level(const std::string& level) {
    spdlog::level::level_enum lv = spdlog::level::info;
    if (level == "debug") lv = spdlog::level::debug;
    else if (level == "warn") lv = spdlog::level::warn;
    else if (level == "error") lv = spdlog::level::err;
    else if (level == "trace") lv = spdlog::level::trace;
    else if (level == "off") lv = spdlog::level::off;
    root_logger_->set_level(lv);
    root_logger_->flush_on(lv);
}

std::shared_ptr<spdlog::logger> Logging::get_logger(const std::string& name) {
    if (!root_logger_) {
        setup("/tmp");
    }
    return spdlog::get(name) ? spdlog::get(name) : root_logger_;
}

} // namespace tv
