// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/write_file_tool.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace pu::tools {

std::string WriteFileTool::Name() const {
  return "write_file";
}

std::string WriteFileTool::Description() const {
  return "Write text content to a file (overwrites if exists). Use this instead of bash redirections.";
}

std::string WriteFileTool::ParametersSchema() const {
  return R"##({
    "type": "object",
    "properties": {
      "path": {"type": "string", "description": "File path (relative to current directory)"},
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

}  // namespace pu::tools
