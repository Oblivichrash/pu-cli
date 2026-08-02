// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "tools/command_executor.hpp"

namespace pu::tests {

class MockCommandExecutor : public pu::executor::CommandExecutor {
 public:
  MockCommandExecutor() : CommandExecutor("") {}

  pu::executor::ExecutionResult Execute(const std::string& command) override {
    last_command = command;
    if (custom_result) {
      return *custom_result;
    }
    pu::executor::ExecutionResult r;
    r.exit_code = 0;
    r.stdout_content = "mock output";
    return r;
  }

  std::string last_command;
  std::optional<pu::executor::ExecutionResult> custom_result;
};

}  // namespace pu::tests