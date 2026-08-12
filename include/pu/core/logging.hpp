// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/formatter.h>

namespace pu {

void InitLogging(const std::string& log_level = "");
void ShutdownLogging();

// Thread-local structured-logging context (used when PU_LOG_JSON=1).
void SetLogSessionId(const std::string& session_id);
void ClearLogSessionId();
void BeginRequest();
void SetLogToolName(const std::string& tool_name);
void ClearLogToolName();
void SetLogDurationMs(int64_t duration_ms);
void ClearLogDurationMs();

// Formats each log record as a single JSON line enriched with the
// thread-local context fields above.
class JsonLogFormatter : public spdlog::formatter {
 public:
  void format(const spdlog::details::log_msg& msg,
              spdlog::memory_buf_t& dest) override;
  std::unique_ptr<spdlog::formatter> clone() const override;
};

}  // namespace pu
