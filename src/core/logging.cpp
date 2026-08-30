// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/logging.hpp"
#include "pu/path_utils.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace pu {

namespace {

thread_local std::string g_session_id;
thread_local std::string g_request_id;
thread_local std::string g_tool_name;
thread_local int64_t g_duration_ms = -1;

std::string GenerateUuid() {
  static thread_local std::mt19937 gen(std::random_device{}());
  std::uniform_int_distribution<int> dist(0, 15);
  const char* hex = "0123456789abcdef";
  std::string uuid(36, '-');
  for (size_t i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
    uuid[i] = hex[dist(gen)];
  }
  uuid[14] = '4';
  uuid[19] = hex[(dist(gen) & 0x3) | 0x8];
  return uuid;
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

std::string FormatTimestamp(const std::chrono::system_clock::time_point& tp) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch());
  auto secs = std::chrono::duration_cast<std::chrono::seconds>(ms);
  auto ms_part = (ms - secs).count();
  std::time_t t = std::chrono::system_clock::to_time_t(tp);
  std::tm tm_buf{};
#ifdef _WIN32
  gmtime_s(&tm_buf, &t);
#else
  gmtime_r(&t, &tm_buf);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
  std::ostringstream oss;
  oss << buf << '.' << std::setw(3) << std::setfill('0') << ms_part << 'Z';
  return oss.str();
}

}  // namespace

void SetLogSessionId(const std::string& session_id) { g_session_id = session_id; }
void ClearLogSessionId() { g_session_id.clear(); }
void BeginRequest() { g_request_id = GenerateUuid(); }
void SetLogRequestId(const std::string& request_id) { g_request_id = request_id; }
void ClearLogRequestId() { g_request_id.clear(); }
void SetLogToolName(const std::string& tool_name) { g_tool_name = tool_name; }
void ClearLogToolName() { g_tool_name.clear(); }
void SetLogDurationMs(int64_t duration_ms) { g_duration_ms = duration_ms; }
void ClearLogDurationMs() { g_duration_ms = -1; }

void JsonLogFormatter::format(const spdlog::details::log_msg& msg,
                              spdlog::memory_buf_t& dest) {
  std::string out = "{";
  out += "\"timestamp\":\"" + FormatTimestamp(msg.time) + "\",";
  auto level_view = spdlog::level::to_string_view(msg.level);
  out += "\"level\":\"" + std::string(level_view.data(), level_view.size()) + "\",";
  if (!g_session_id.empty()) out += "\"session_id\":\"" + JsonEscape(g_session_id) + "\",";
  if (!g_request_id.empty()) out += "\"request_id\":\"" + JsonEscape(g_request_id) + "\",";
  if (!g_tool_name.empty()) out += "\"tool_name\":\"" + JsonEscape(g_tool_name) + "\",";
  if (g_duration_ms >= 0) out += "\"duration_ms\":" + std::to_string(g_duration_ms) + ",";
  out += "\"message\":\"" +
         JsonEscape(std::string(msg.payload.data(), msg.payload.size())) + "\"";
  out += "}\n";
  dest.append(out.data(), out.data() + out.size());
}

std::unique_ptr<spdlog::formatter> JsonLogFormatter::clone() const {
  return std::make_unique<JsonLogFormatter>();
}

void InitLogging(const std::string& log_level, bool /*trace_enabled*/) {
  spdlog::level::level_enum level = spdlog::level::info;
  if (!log_level.empty()) {
    if (log_level == "trace") level = spdlog::level::trace;
    else if (log_level == "debug") level = spdlog::level::debug;
    else if (log_level == "info") level = spdlog::level::info;
    else if (log_level == "warn") level = spdlog::level::warn;
    else if (log_level == "error") level = spdlog::level::err;
    else if (log_level == "critical") level = spdlog::level::critical;
  }

  std::vector<spdlog::sink_ptr> sinks;

  // The console sink is always present but only emits error/critical records.
  // Info/warn/trace/debug are intentionally never written to the console so
  // that normal use stays quiet while failures remain visible.
  auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
  console_sink->set_level(spdlog::level::err);
  sinks.push_back(console_sink);

  std::filesystem::path log_dir = pu::path::GetDataDir() / "logs";
  std::filesystem::create_directories(log_dir);
  auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
      (log_dir / "pu.log").string(), 1024 * 1024 * 5, 3);
  file_sink->set_level(level);
  sinks.push_back(file_sink);

  const char* json_env = std::getenv("PU_LOG_JSON");
  bool use_json = json_env && std::string(json_env) == "1";

  std::shared_ptr<spdlog::logger> logger;
  if (use_json) {
    logger = std::make_shared<spdlog::logger>("pu", sinks.begin(), sinks.end());
  } else {
    spdlog::init_thread_pool(8192, 1);
    logger = std::make_shared<spdlog::async_logger>(
        "pu", sinks.begin(), sinks.end(),
        spdlog::thread_pool(), spdlog::async_overflow_policy::block);
  }
  logger->set_level(level);
  logger->flush_on(spdlog::level::err);
  if (use_json) {
    logger->set_formatter(std::make_unique<JsonLogFormatter>());
  }
  spdlog::set_default_logger(logger);
}

void ShutdownLogging() {
  spdlog::shutdown();
}

} // namespace pu
