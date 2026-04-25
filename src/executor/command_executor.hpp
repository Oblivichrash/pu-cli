// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
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

  // Execute command, returns combined output and exit code
  ExecutionResult Execute(const std::string& command);

  // Check if a command matches dangerous patterns
  bool IsDangerous(const std::string& command, std::string* reason = nullptr) const;

 private:
  std::string sandbox_path_;
  static const std::vector<std::string> dangerous_patterns_;
};

}  // namespace pu::executor
