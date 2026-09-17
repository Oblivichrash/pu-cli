// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/builtin_tools.hpp"

#include "pu/core/platform.hpp"
#include "pu/tools/tool_result.hpp"
#include "pu/core/json.hpp"

#include <boost/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <string>

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
  RiskLevel level = RiskLevel::kSafe;
  std::string reason;
};

struct CommandResult {
  int exit_code = 0;
  std::string stdout_content;
  std::string stderr_content;
  bool was_intercepted = false;
  std::string intercept_reason;
};

class CommandExecutor {
 public:
  explicit CommandExecutor(std::string sandbox_path)
      : sandbox_path_(std::move(sandbox_path)) {}

  RiskAssessment AssessRisk(const std::string& command) const {
    RiskAssessment result;
    std::string pattern;
    if (MatchAnyPattern(command, dangerous_patterns_, &pattern)) {
      result.level = RiskLevel::kDangerous;
      result.reason = "Matches dangerous pattern: " + pattern;
      return result;
    }
    result.level = MatchAnyPattern(command, safe_commands_) ? RiskLevel::kSafe
                                                           : RiskLevel::kNeutral;
    return result;
  }

  CommandResult Execute(const std::string& command) {
    CommandResult result;
    auto risk = AssessRisk(command);
    if (risk.level == RiskLevel::kDangerous) {
      result.was_intercepted = true;
      result.intercept_reason = risk.reason;
      result.exit_code = -1;
      return result;
    }

    std::string output;
    int exit_code = pu::platform::ExecuteCommand(command, output, sandbox_path_);
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
  return "Execute a shell command.";
}

boost::json::value ExecuteBashToolStandard::ParametersSchema() const {
  return boost::json::object{
      {"type", "object"},
      {"properties",
       boost::json::object{
           {"command",
            boost::json::object{
                {"type", "string"},
                {"description", "The shell command to execute"},
            }},
       }},
      {"required", boost::json::array{"command"}},
  };
}

std::string ExecuteBashToolStandard::Execute(const boost::json::value& args, pu::ToolContext& ctx) {
  std::string command;
  if (args.is_object() && json::HasKey(args, "command")) {
    command = boost::json::value_to<std::string>(args.at("command"));
  }
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
      std::regex re("\\b" + pattern + "\\b");
      if (std::regex_search(command, re)) {
        return tools::MakeToolResultJson(false, "", "",
                            "command contains forbidden pattern '" + pattern + "'", -1);
      }
    }
  }

  CommandExecutor executor(sandbox_root_);
  auto risk = executor.AssessRisk(command);
  if (risk.level == RiskLevel::kDangerous) {
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
  return "Write text to a file.";
}

boost::json::value WriteFileTool::ParametersSchema() const {
  return boost::json::object{
      {"type", "object"},
      {"properties",
       boost::json::object{
           {"path",
            boost::json::object{
                {"type", "string"},
                {"description", "File path (relative to sandbox)"},
            }},
           {"content",
            boost::json::object{
                {"type", "string"},
                {"description", "Text to write"},
            }},
       }},
      {"required", boost::json::array{"path", "content"}},
  };
}

std::string WriteFileTool::Execute(const boost::json::value& args, pu::ToolContext& ctx) {
  std::string path = json::ValueOrDefault<std::string>(args, "path", "");
  std::string content = json::ValueOrDefault<std::string>(args, "content", "");
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

std::string AskUserTool::Name() const {
  return "ask_user";
}

std::string AskUserTool::Description() const {
  return "Ask user for clarification.";
}

boost::json::value AskUserTool::ParametersSchema() const {
  return boost::json::object{
      {"type", "object"},
      {"properties",
       boost::json::object{
           {"question",
            boost::json::object{
                {"type", "string"},
                {"description", "The question to ask"},
            }},
       }},
      {"required", boost::json::array{"question"}},
  };
}

std::string AskUserTool::Execute(const boost::json::value& args, pu::ToolContext& ctx) {
  (void)ctx;
  boost::json::value result = {
    {"success", false},
    {"error", "clarification_needed"},
  };

  std::string question;
  if (args.is_object() && json::HasKey(args, "question")) {
    question = boost::json::value_to<std::string>(args.at("question"));
  }
  result.as_object()["question"] = question;

  return boost::json::serialize(result);
}

}  // namespace pu::tools
