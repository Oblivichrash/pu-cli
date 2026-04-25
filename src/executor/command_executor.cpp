// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.

#include "command_executor.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <regex>
#include <stdexcept>
#include <array>

#ifdef _WIN32
// Windows popen/pclose compatibility
#define popen _popen
#define pclose _pclose

// Extract exit code from _pclose return value (simplified)
static int GetExitCodeFromPClose(int status) {
    // On Windows, _pclose returns the exit code of the cmd process.
    return (status == -1) ? -1 : status;
}
#else
#include <sys/wait.h>  // for WEXITSTATUS
#define GetExitCodeFromPClose(status) WEXITSTATUS(status)
#endif

namespace pu::executor {

const std::vector<std::string> CommandExecutor::dangerous_patterns_ = {
    R"(rm\s+-rf\s+/)",
    R"(sudo\b)",
    R"(mkfs)",
    R"(dd\s+if=.*of=/dev/sd)",
    R"(:\(\)\{ :\|:&\};:)",
};

CommandExecutor::CommandExecutor(std::string sandbox_path)
    : sandbox_path_(std::move(sandbox_path)) {}

bool CommandExecutor::IsDangerous(const std::string& command,
                                  std::string* reason) const {
  for (const auto& pattern : dangerous_patterns_) {
    try {
      std::regex re(pattern, std::regex::icase);
      if (std::regex_search(command, re)) {
        if (reason) {
          *reason = "Matches dangerous pattern: " + pattern;
        }
        return true;
      }
    } catch (const std::regex_error&) {
      // ignore malformed patterns
    }
  }
  return false;
}

ExecutionResult CommandExecutor::Execute(const std::string& command) {
  ExecutionResult result;

  std::string reason;
  if (IsDangerous(command, &reason)) {
    result.was_intercepted = true;
    result.intercept_reason = reason;
    result.exit_code = -1;
    return result;
  }

  // Build command with sandbox path
  std::string full_cmd = command;
  if (!sandbox_path_.empty()) {
    full_cmd = "cd " + sandbox_path_ + " && " + command + " 2>&1";
  }
  FILE* pipe = popen(full_cmd.c_str(), "r");
  if (!pipe) {
    result.exit_code = -1;
    result.stderr_content = "popen failed";
    return result;
  }

  std::array<char, 256> buffer;
  std::string output;
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }

  int status = pclose(pipe);
  result.exit_code = GetExitCodeFromPClose(status);
  result.stdout_content = output;
  return result;
}

}  // namespace pu::executor
