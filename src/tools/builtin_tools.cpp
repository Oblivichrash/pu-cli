// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/builtin_tools.hpp"

#include "pu/agent/agent_manager.hpp"
#include "pu/infra/platform.hpp"
#include "pu/tools/tool_result.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <string>

#ifdef _WIN32
#  include <direct.h>  // for _chdir
#  define chdir _chdir
#else
#  include <unistd.h>  // for chdir
#endif

namespace pu::tools {

namespace {

bool MatchAnyPattern(const std::string& command, const std::vector<std::string>& patterns,
                     std::string* matched = nullptr) {
  for (const auto& pattern : patterns) {
    try {
      if (std::regex_search(command, std::regex(pattern, std::regex::icase))) {
        if (matched) *matched = pattern;
        return true;
      }
    } catch (const std::regex_error&) {}
  }
  return false;
}

struct RiskAssessment {
  pu::executor::RiskLevel level = pu::executor::RiskLevel::kSafe;
  std::string reason;
};

struct CommandResult {
  int exit_code = 0;
  std::string stdout_content;
  std::string stderr_content;
  bool was_intercepted = false;
  std::string intercept_reason;
};

// Sandboxed command executor, previously src/tools/command_executor.*. It is
// kept as an internal detail of builtin_tools.cpp since ExecuteBashToolStandard
// is its only consumer.
class CommandExecutor {
 public:
  explicit CommandExecutor(std::string sandbox_path)
      : sandbox_path_(std::move(sandbox_path)) {}

  RiskAssessment AssessRisk(const std::string& command) const {
    RiskAssessment result;
    std::string pattern;
    if (MatchAnyPattern(command, dangerous_patterns_, &pattern)) {
      result.level = pu::executor::RiskLevel::kDangerous;
      result.reason = "Matches dangerous pattern: " + pattern;
      return result;
    }
    result.level = MatchAnyPattern(command, safe_commands_)
                       ? pu::executor::RiskLevel::kSafe
                       : pu::executor::RiskLevel::kNeutral;
    return result;
  }

  CommandResult Execute(const std::string& command) {
    CommandResult result;
    auto risk = AssessRisk(command);
    if (risk.level == pu::executor::RiskLevel::kDangerous) {
      result.was_intercepted = true;
      result.intercept_reason = risk.reason;
      result.exit_code = -1;
      return result;
    }

    // Switch to the sandbox directory if specified.
    if (!sandbox_path_.empty()) {
      if (chdir(sandbox_path_.c_str()) != 0) {
        result.exit_code = -1;
        result.stderr_content = "Failed to chdir to sandbox: " + sandbox_path_;
        return result;
      }
    }

    std::string output;
    int exit_code = pu::platform::ExecuteCommand(command, output);
    result.exit_code = exit_code;
    result.stdout_content = output;
    if (exit_code != 0) result.stderr_content = output;
    return result;
  }

 private:
  std::string sandbox_path_;

  static const std::vector<std::string> dangerous_patterns_;
  static const std::vector<std::string> safe_commands_;
};

const std::vector<std::string> CommandExecutor::dangerous_patterns_ = {
    R"(rm\s+-rf\s+/)", R"(sudo\b)", R"(mkfs)",
    R"(dd\s+if=.*of=/dev/sd)", R"(:\(\)\{ :\|:&\};:)" };

const std::vector<std::string> CommandExecutor::safe_commands_ = {
    "ls", "pwd", "cat", "head", "tail", "less", "more",
    "echo", "date", "whoami", "hostname", "uptime",
    "which", "type", "wc", "sort", "uniq", "cut", "tr",
    "find .", "grep", "awk", "sed", "diff", "file",
    "stat", "du", "df", "free", "ps", "top -n", "pgrep" };

}  // namespace

ExecuteBashToolStandard::ExecuteBashToolStandard(std::string sandbox_root)
    : sandbox_root_(std::move(sandbox_root)) {}

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
    return tools::MakeToolResultJson(false, "", "", "'command' parameter is required", -1);
  }

  if (ctx.security && ctx.security->max_command_length > 0 && command.size() > ctx.security->max_command_length) {
    return tools::MakeToolResultJson(false, "", "",
                        "command exceeds maximum allowed length (" +
                            std::to_string(ctx.security->max_command_length) + ")",
                        -1);
  }

  if (ctx.security) {
    for (const auto& pattern : ctx.security->forbidden_patterns) {
      // Word-boundary matching prevents false positives where a short pattern
      // such as "dd" or "rm" appears as a substring inside a Git commit hash
      // or file path.
      std::regex re("\\b" + pattern + "\\b");
      if (std::regex_search(command, re)) {
        return tools::MakeToolResultJson(false, "", "",
                            "command contains forbidden pattern '" + pattern + "'", -1);
      }
    }
  }

  CommandExecutor executor(sandbox_root_);
  auto risk = executor.AssessRisk(command);
  if (risk.level == pu::executor::RiskLevel::kDangerous) {
    return tools::MakeToolResultJson(false, "", "", "Blocked: " + risk.reason, -1);
  }

  auto result = executor.Execute(command);
  if (result.was_intercepted) {
    return tools::MakeToolResultJson(false, "", "", "Blocked: " + result.intercept_reason, -1);
  }

  if (result.exit_code == 0) {
    return tools::MakeToolResultJson(true, result.stdout_content, result.stderr_content, "", 0);
  } else {
    return tools::MakeToolResultJson(false, result.stdout_content, result.stderr_content,
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
    return tools::MakeToolResultJson(false, "", "", "'path' is required", -1);
  }

  if (!ctx.security) {
    return tools::MakeToolResultJson(false, "", "", "security policy not set", -1);
  }

  std::error_code ec;
  std::filesystem::path sandbox_root(ctx.security->sandbox_root);
  auto sandbox_canonical = std::filesystem::weakly_canonical(sandbox_root, ec);
  if (ec) {
    return tools::MakeToolResultJson(false, "", "",
                        "cannot resolve sandbox root: " + ctx.security->sandbox_root, -1);
  }

  std::filesystem::path full_path = sandbox_canonical / path;
  full_path = std::filesystem::weakly_canonical(full_path, ec);
  if (ec) {
    return tools::MakeToolResultJson(false, "", "", "invalid path", -1);
  }

  auto target_str = full_path.string();
  auto sandbox_str = sandbox_canonical.string();
  if (target_str.find(sandbox_str) != 0) {
    return tools::MakeToolResultJson(false, "", "", "path outside sandbox root (traversal not allowed)", -1);
  }

  std::filesystem::create_directories(full_path.parent_path(), ec);
  if (ec) {
    return tools::MakeToolResultJson(false, "", "", "cannot create parent directories", -1);
  }

  std::ofstream file(full_path);
  if (!file.is_open()) {
    return tools::MakeToolResultJson(false, "", "", "cannot write to " + path, -1);
  }
  file << content;

  std::string summary = "Successfully wrote " + std::to_string(content.size()) + " bytes to " + path;
  return tools::MakeToolResultJson(true, summary, "", "", 0);
}

}  // namespace pu::tools
