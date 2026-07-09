// SPDX-License-Identifier: GPL-3.0-only
#include "tools/create_tool.hpp"
#include <fstream>
#include <filesystem>
#include <regex>
#include <iostream>
#include <nlohmann/json.hpp>

namespace pu::tools {

std::string CreateTool::Name() const {
  return "create_tool";
}

std::string CreateTool::Description() const {
  return "Create a new Python tool. Provide name, description, parameters schema, and Python code. "
         "The tool will be saved and immediately available for use.";
}

std::string CreateTool::ParametersSchema() const {
  return R"JSON({
    "type": "object",
    "properties": {
      "name": {"type": "string", "description": "Unique tool name (alphanumeric, underscore allowed)"},
      "description": {"type": "string", "description": "Brief description of what the tool does"},
      "parameters_schema": {"type": "string", "description": "JSON Schema for tool parameters"},
      "python_code": {"type": "string", "description": "Python code containing a 'run' function"}
    },
    "required": ["name", "description", "parameters_schema", "python_code"]
  })JSON";
}

static bool IsSafePythonCode(const std::string& code) {
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
