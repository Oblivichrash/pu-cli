// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/write_file_tool.hpp"
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace pu::tools {

std::string WriteFileTool::Name() const {
  return "write_file";
}

std::string WriteFileTool::Description() const {
  return "Write text content to a file (overwrites if exists). Use this instead of bash redirections.";
}

std::string WriteFileTool::ParametersSchema() const {
  return R"({
    "type": "object",
    "properties": {
      "path": {"type": "string", "description": "File path (relative to current directory)"},
      "content": {"type": "string", "description": "Text content to write"}
    },
    "required": ["path", "content"]
  })";
}

std::string WriteFileTool::Execute(const nlohmann::json& args, agent::ToolContext& ctx) {
  std::string path = args.value("path", "");
  std::string content = args.value("content", "");
  if (path.empty()) return "Error: 'path' is required";

  std::filesystem::path full_path = std::filesystem::current_path() / path;

  if (!ctx.sandbox_root.empty()) {
    std::filesystem::path sandbox = std::filesystem::canonical(ctx.sandbox_root);
    std::filesystem::path target = std::filesystem::canonical(full_path.parent_path());
    if (target.string().find(sandbox.string()) != 0) {
      return "Error: path outside sandbox";
    }
  }

  for (const auto& allowed : ctx.allowed_paths) {
    if (!allowed.empty()) {
      std::filesystem::path allowed_path = std::filesystem::canonical(allowed);
      if (full_path.string().find(allowed_path.string()) == 0) {
        goto allowed;
      }
    }
  }
  if (!ctx.allowed_paths.empty()) {
    return "Error: path not in allowed paths list";
  }
allowed:

  std::ofstream file(full_path);
  if (!file.is_open()) {
    return "Error: cannot write to " + path;
  }
  file << content;
  return "Successfully wrote " + std::to_string(content.size()) + " bytes to " + path;
}

}  // namespace pu::tools
