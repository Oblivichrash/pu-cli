#include "pu/core/logging.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <cstdlib>
#include <filesystem>
#include <vector>

namespace pu {

void InitLogging(const std::string& log_level, bool trace_enabled) {
    // 1. Determine log level
    spdlog::level::level_enum level = spdlog::level::info;
    if (trace_enabled || std::getenv("PU_TRACE")) {
        level = spdlog::level::trace;
    } else if (!log_level.empty()) {
        if (log_level == "trace") level = spdlog::level::trace;
        else if (log_level == "debug") level = spdlog::level::debug;
        else if (log_level == "info") level = spdlog::level::info;
        else if (log_level == "warn") level = spdlog::level::warn;
        else if (log_level == "error") level = spdlog::level::err;
        else if (log_level == "critical") level = spdlog::level::critical;
    }

    // 2. Create sinks
    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    console_sink->set_level(level);

    // 3. File sink (rotating log, max 5MB, keep 3 files)
    std::filesystem::path log_dir = std::getenv("HOME")
        ? std::filesystem::path(std::getenv("HOME")) / ".pu" / "logs"
        : "/tmp/pu_logs";
    std::filesystem::create_directories(log_dir);
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        (log_dir / "pu.log").string(), 1024 * 1024 * 5, 3);

    // 4. Combine sinks
    std::vector<spdlog::sink_ptr> sinks = {console_sink, file_sink};

    // 5. Create async logger
    spdlog::init_thread_pool(8192, 1);
    auto logger = std::make_shared<spdlog::async_logger>(
        "pu", sinks.begin(), sinks.end(),
        spdlog::thread_pool(), spdlog::async_overflow_policy::block);
    logger->set_level(level);
    logger->flush_on(spdlog::level::err);
    spdlog::set_default_logger(logger);
}

void ShutdownLogging() {
    spdlog::shutdown();
}

} // namespace pu