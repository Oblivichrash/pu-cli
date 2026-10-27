// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/formatter.h>

namespace pu {

// The one directory PU_HOME moves, and the only thing it moves: logs. Session and
// agent state stay under the workspace's own .pu/.
std::filesystem::path LogDir();

void InitLogging(const std::string& log_level = "");
void FlushLogging();

void BeginRequest();
void ClearLogRequestId();
void SetLogToolName(const std::string& tool_name);
void ClearLogToolName();
void SetLogDurationMs(int64_t duration_ms);
void ClearLogDurationMs();

class JsonLogFormatter : public spdlog::formatter {
 public:
  void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override;
  std::unique_ptr<spdlog::formatter> clone() const override;
};

}  // namespace pu