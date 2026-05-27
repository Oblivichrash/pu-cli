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
  full_path = std::filesystem::weakly_canonical(full_path);

  if (!ctx.sandbox_root.empty()) {
    std::filesystem::path sandbox = std::filesystem::weakly_canonical(ctx.sandbox_root);
    if (full_path.string().find(sandbox.string()) != 0) {
      return "Error: path outside sandbox";
    }
  }

  if (!ctx.allowed_paths.empty()) {
    bool allowed = false;
    for (const auto& allowed_str : ctx.allowed_paths) {
      std::filesystem::path allowed_path = std::filesystem::weakly_canonical(allowed_str);
      if (full_path.string().find(allowed_path.string()) == 0) {
        allowed = true;
        break;
      }
    }
    if (!allowed) {
      return "Error: path not in allowed directories";
    }
  }

  std::ofstream file(full_path);
  if (!file.is_open()) {
    return "Error: cannot write to " + path;
  }
  file << content;
  return "Successfully wrote " + std::to_string(content.size()) + " bytes to " + path;
}
