// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/execute_bash_tool.hpp"
#include "pu/tools/write_file_tool.hpp"
#include "pu/tools/create_tool.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>

namespace pu::tools {

// ─── ExecuteBashToolStandard ────────────────────────────────────────────────

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

std::string ExecuteBashToolStandard::Execute(const nlohmann::json& args, agent::ToolContext& ctx) {
  (void)ctx;
  std::string command = args.value("command", "");
  if (command.empty()) {
    return "Error: 'command' parameter is required";
  }

  if (ctx.security && ctx.security->max_command_length > 0 && command.size() > ctx.security->max_command_length) {
    return "Error: command exceeds maximum allowed length (" + std::to_string(ctx.security->max_command_length) + ")";
  }

  for (const auto& pattern : ctx.security->forbidden_patterns) {
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

// ─── WriteFileTool ──────────────────────────────────────────────────────────

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

std::string WriteFileTool::Execute(const nlohmann::json& args, agent::ToolContext& ctx) {
  std::string path = args.value("path", "");
  std::string content = args.value("content", "");
  if (path.empty()) return "Error: 'path' is required";

  if (!ctx.security) return "Error: security policy not set";

  std::error_code ec;
  std::filesystem::path sandbox_root(ctx.security->sandbox_root);
  auto sandbox_canonical = std::filesystem::weakly_canonical(sandbox_root, ec);
  if (ec) return "Error: cannot resolve sandbox root: " + ctx.security->sandbox_root;

  std::filesystem::path full_path = sandbox_canonical / path;
  full_path = std::filesystem::weakly_canonical(full_path, ec);
  if (ec) return "Error: invalid path";

  auto target_str = full_path.string();
  auto sandbox_str = sandbox_canonical.string();
  if (target_str.find(sandbox_str) != 0) {
    return "Error: path outside sandbox root (traversal not allowed)";
  }

  std::filesystem::create_directories(full_path.parent_path(), ec);
  if (ec) return "Error: cannot create parent directories";

  std::ofstream file(full_path);
  if (!file.is_open()) return "Error: cannot write to " + path;
  file << content;
  return "Successfully wrote " + std::to_string(content.size()) + " bytes to " + path;
}

// ─── CreateTool ─────────────────────────────────────────────────────────────

namespace {

bool IsSafePythonCode(const std::string& code) {
  std::vector<std::string> dangerous_patterns = {
    "os.system", "subprocess", "eval(", "exec(", "__import__", "open(", "file(",
    "execfile", "compile(", "globals()", "locals()", "__builtins__"
  };
  for (const auto& pat : dangerous_patterns) {
    if (code.find(pat) != std::string::npos) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string CreateTool::Name() const {
  return "create_tool";
}

std::string CreateTool::Description() const {
  return "Create a new Python tool with name, schema, and code.";
}

std::string CreateTool::ParametersSchema() const {
  return R"JSON({
    "type": "object",
    "properties": {
      "name": {"type": "string", "description": "Unique tool name (alphanumeric, underscore)"},
      "description": {"type": "string", "description": "Brief description of the tool"},
      "parameters_schema": {"type": "string", "description": "JSON Schema for tool parameters"},
      "python_code": {"type": "string", "description": "Python code with a 'run' function"}
    },
    "required": ["name", "description", "parameters_schema", "python_code"]
  })JSON";
}

std::string CreateTool::Execute(const nlohmann::json& args, agent::ToolContext& ctx) {
  (void)ctx;
  std::string name = args.value("name", "");
  std::string description = args.value("description", "");
  std::string parameters_schema = args.value("parameters_schema", "");
  std::string python_code = args.value("python_code", "");

  if (name.empty() || description.empty() || parameters_schema.empty() || python_code.empty()) {
    return "Error: missing required fields";
  }

  if (!std::regex_match(name, std::regex("^[a-zA-Z_][a-zA-Z0-9_]*$"))) {
    return "Error: invalid tool name (must be alphanumeric and start with letter or underscore)";
  }

  if (!IsSafePythonCode(python_code)) {
    return "Error: Python code contains unsafe operations (os.system, subprocess, eval, etc.)";
  }

  const char* home = std::getenv("HOME");
  if (!home) return "Error: HOME environment variable not set";
  std::filesystem::path tools_dir = std::filesystem::path(home) / ".pu" / "tools";
  std::filesystem::create_directories(tools_dir);

  std::filesystem::path file_path = tools_dir / (name + ".py");

  std::ofstream file(file_path);
  if (!file.is_open()) {
    return "Error: cannot write to " + file_path.string();
  }

  file << "# tool: " << name << "\n";
  file << "# description: " << description << "\n";
  file << "# parameters: " << parameters_schema << "\n\n";
  file << python_code << "\n";
  file.close();

  if (ctx.request_confirmation) {
    std::string confirm_msg = "Created new tool '" + name + "'. Do you want to reload tools now? (y/n)";
    if (!ctx.request_confirmation(confirm_msg)) {
      return "Tool saved but not reloaded. Use /reload-tools to activate.";
    }
  }

  return "Tool '" + name + "' created successfully. Use /reload-tools to activate, or restart the agent.";
}

}  // namespace pu::tools
