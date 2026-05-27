// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/execute_bash_tool.hpp"
#include <nlohmann/json.hpp>

namespace pu::tools {

ExecuteBashToolStandard::ExecuteBashToolStandard(std::unique_ptr<executor::CommandExecutor> executor)
    : executor_(std::move(executor)) {}

std::string ExecuteBashToolStandard::Name() const {
  return "execute_bash";
}

std::string ExecuteBashToolStandard::Description() const {
  return "Execute a safe Linux bash command and return the output. Use this tool for any command-line operation.";
}

std::string ExecuteBashToolStandard::ParametersSchema() const {
  return R"({
    "type": "object",
    "properties": {
      "command": {
        "type": "string",
        "description": "The bash command to execute"
      }
    },
    "required": ["command"]
  })";
}

std::string ExecuteBashToolStandard::Execute(const nlohmann::json& args, agent::ToolContext& ctx) {
  std::string command = args.value("command", "");
  if (command.empty()) {
    return "Error: 'command' parameter is required";
  }

  if (ctx.max_command_length > 0 && command.size() > ctx.max_command_length) {
    return "Error: command exceeds maximum allowed length (" + std::to_string(ctx.max_command_length) + ")";
  }

  for (const auto& pattern : ctx.forbidden_patterns) {
    if (command.find(pattern) != std::string::npos) {
      return "Blocked: command contains forbidden pattern '" + pattern + "'";
    }
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
