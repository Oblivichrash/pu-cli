// SPDX-License-Identifier: GPL-3.0-only

#include "command_executor.hpp"
#include "platform/platform.hpp"

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
  return pu::platform::IsDangerous(command, reason);
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

  std::string full_command = command;
  if (!sandbox_path_.empty()) {
    full_command = "cd " + sandbox_path_ + " && " + command;
  }

  std::string output;
  int exit_code = pu::platform::ExecuteCommand(full_command, output);

  result.exit_code = exit_code;
  result.stdout_content = output;
  if (exit_code != 0) {
    result.stderr_content = output;  // stderr merged
  }
  return result;
}

}  // namespace pu::executor
