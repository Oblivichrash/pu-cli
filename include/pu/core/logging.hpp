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

// Where the current run happens, which is what a relative path in `LogDir` and
// `FindConfigPath` is read against. A run with `-C` is about that directory, so its logs and
// its agents.json have to be found there and not where the binary happened to be started --
// otherwise the same command run from two shells writes to two different places. Set once,
// as early as the workspace root is known; a run that never sets it keeps the process's
// working directory, which is what a tool invoked from inside a workspace has always meant.
void SetWorkspaceRoot(const std::filesystem::path& root);
std::filesystem::path WorkspaceRoot();

void InitLogging(const std::string& log_level = "");
// True once any Runtime in this process has installed a logger. A nested Runtime built for
// a delegated turn asks this before calling InitLogging: the process has one log destination
// and one level, and whoever installed it first owns both.
bool IsLoggingInitialized();
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