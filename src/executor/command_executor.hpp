// SPDX-License-Identifier: GPL-3.0-only
//
// Simple command executor for Linux/WSL with dangerous pattern checks.

#pragma once

#include <string>
#include <vector>

namespace pu::executor {

struct ExecutionResult {
  int exit_code = 0;
  std::string stdout_content;
  std::string stderr_content;
  bool was_intercepted = false;
  std::string intercept_reason;
};

class CommandExecutor {
 public:
  explicit CommandExecutor(std::string sandbox_path);

  ExecutionResult Execute(const std::string& command);

  bool IsDangerous(const std::string& command, std::string* reason = nullptr) const;

 private:
  std::string sandbox_path_;
  static const std::vector<std::string> dangerous_patterns_;
};

}  // namespace pu::executor
