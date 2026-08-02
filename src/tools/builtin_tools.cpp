// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/execute_bash_tool.hpp"
#include "pu/tools/write_file_tool.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>

namespace pu::tools {

namespace {

std::string MakeToolJson(bool success,
                         const std::string& stdout_str,
                         const std::string& stderr_str,
                         const std::string& error_str,
                         int exit_code) {
  nlohmann::json j;
  j["success"] = success;
  j["stdout"] = stdout_str;
  j["stderr"] = stderr_str;
  j["error"] = error_str;
  j["exit_code"] = exit_code;
  return j.dump();
}

}  // namespace

ExecuteBashToolStandard::ExecuteBashToolStandard(std::unique_ptr<executor::CommandExecutor> executor)
    : executor_(std::move(executor)) {}

std::string ExecuteBashToolStandard::Name() const {
  return "execute_bash";
}

std::string ExecuteBashToolStandard::Description() const {
  return "Execute a bash command and return output.";
}

std::string ExecuteBashToolStandard::ParametersSchema() const {
  return R"##({
    "type": "object",
    "properties": {
      "command": {
        "type": "string",
        "description": "The bash command to execute"
      }
    },
    "required": ["command"]
  })##";
}

std::string ExecuteBashToolStandard::Execute(const nlohmann::json& args, pu::ToolContext& ctx) {
  std::string command = args.value("command", "");
  if (command.empty()) {
    return MakeToolJson(false, "", "", "'command' parameter is required", -1);
  }

  if (ctx.security && ctx.security->max_command_length > 0 && command.size() > ctx.security->max_command_length) {
    return MakeToolJson(false, "", "",
                        "command exceeds maximum allowed length (" +
                            std::to_string(ctx.security->max_command_length) + ")",
                        -1);
  }

  if (ctx.security) {
    for (const auto& pattern : ctx.security->forbidden_patterns) {
      if (command.find(pattern) != std::string::npos) {
        return MakeToolJson(false, "", "",
                            "command contains forbidden pattern '" + pattern + "'", -1);
      }
    }
  }

  auto risk = executor_->AssessRisk(command);
  if (risk.level == executor::RiskLevel::kDangerous) {
    return MakeToolJson(false, "", "", "Blocked: " + risk.reason, -1);
  }

  auto result = executor_->Execute(command);
  if (result.was_intercepted) {
    return MakeToolJson(false, "", "", "Blocked: " + result.intercept_reason, -1);
  }

  if (result.exit_code == 0) {
    return MakeToolJson(true, result.stdout_content, result.stderr_content, "", 0);
  } else {
    return MakeToolJson(false, result.stdout_content, result.stderr_content,
                        "Command failed (exit " + std::to_string(result.exit_code) + ")",
                        result.exit_code);
  }
}

std::string WriteFileTool::Name() const {
  return "write_file";
}

std::string WriteFileTool::Description() const {
  return "Write text content to a file (overwrites if exists).";
}

std::string WriteFileTool::ParametersSchema() const {
  return R"##({
    "type": "object",
    "properties": {
      "path": {"type": "string", "description": "File path (relative to current dir)"},
      "content": {"type": "string", "description": "Text content to write"}
    },
    "required": ["path", "content"]
  })##";
}

std::string WriteFileTool::Execute(const nlohmann::json& args, pu::ToolContext& ctx) {
  std::string path = args.value("path", "");
  std::string content = args.value("content", "");
  if (path.empty()) {
    return MakeToolJson(false, "", "", "'path' is required", -1);
  }

  if (!ctx.security) {
    return MakeToolJson(false, "", "", "security policy not set", -1);
  }

  std::error_code ec;
  std::filesystem::path sandbox_root(ctx.security->sandbox_root);
  auto sandbox_canonical = std::filesystem::weakly_canonical(sandbox_root, ec);
  if (ec) {
    return MakeToolJson(false, "", "",
                        "cannot resolve sandbox root: " + ctx.security->sandbox_root, -1);
  }

  std::filesystem::path full_path = sandbox_canonical / path;
  full_path = std::filesystem::weakly_canonical(full_path, ec);
  if (ec) {
    return MakeToolJson(false, "", "", "invalid path", -1);
  }

  auto target_str = full_path.string();
  auto sandbox_str = sandbox_canonical.string();
  if (target_str.find(sandbox_str) != 0) {
    return MakeToolJson(false, "", "", "path outside sandbox root (traversal not allowed)", -1);
  }

  std::filesystem::create_directories(full_path.parent_path(), ec);
  if (ec) {
    return MakeToolJson(false, "", "", "cannot create parent directories", -1);
  }

  std::ofstream file(full_path);
  if (!file.is_open()) {
    return MakeToolJson(false, "", "", "cannot write to " + path, -1);
  }
  file << content;

  std::string summary = "Successfully wrote " + std::to_string(content.size()) + " bytes to " + path;
  return MakeToolJson(true, summary, "", "", 0);
}

}  // namespace pu::tools
