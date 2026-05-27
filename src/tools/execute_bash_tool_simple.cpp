// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/execute_bash_tool_simple.hpp"
#include <nlohmann/json.hpp>

namespace pu::tools {

ExecuteBashToolSimple::ExecuteBashToolSimple(std::unique_ptr<executor::CommandExecutor> executor)
    : executor_(std::move(executor)) {}

std::string ExecuteBashToolSimple::Name() const {
  return "execute_bash";
}

std::string ExecuteBashToolSimple::Description() const {
  return "Execute a single, simple bash command. Do not use pipes, redirects, or complex expressions. "
         "For listing files, use 'ls'; for reading files, use 'cat <file>'. Returns the command output.";
}

std::string ExecuteBashToolSimple::ParametersSchema() const {
  return R"({
    "type": "object",
    "properties": {
      "command": {
        "type": "string",
        "description": "A simple bash command without pipes or complex syntax"
      }
    },
    "required": ["command"]
  })";
}

std::string ExecuteBashToolSimple::Execute(const nlohmann::json& args, agent::ToolContext& ctx) {
  std::string command = args.value("command", "");
  if (command.empty()) {
    return "Error: 'command' parameter is required";
  }

  auto risk = executor_->AssessRisk(command);
  if (risk.level == executor::RiskLevel::kDangerous) {
    return "Blocked: " + risk.reason;
  }

  auto result = executor_->Execute(command);
  if (result.was_intercepted) {
    return "Blocked: " + result.intercept_reason;
  }
  if (result.exit_code == 0) {
    return result.stdout_content.empty() ? "Command executed successfully." : result.stdout_content;
  } else {
    return "Command failed (exit " + std::to_string(result.exit_code) + ").\n" +
           result.stderr_content + result.stdout_content;
  }
}

}  // namespace pu::tools
