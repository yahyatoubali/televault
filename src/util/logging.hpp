#pragma once

#include <string>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace tv {

class Logging {
public:
    static void setup(const std::string& log_dir, const std::string& level = "info");
    static std::shared_ptr<spdlog::logger> get_logger(const std::string& name);
    static void set_level(const std::string& level);

private:
    static std::shared_ptr<spdlog::logger> root_logger_;
};

} // namespace tv
