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
  (void)ctx;
  std::string path = args.value("path", "");
  std::string content = args.value("content", "");
  if (path.empty()) return "Error: 'path' is required";

  std::error_code ec;
  std::filesystem::path cwd = std::filesystem::current_path(ec);
  if (ec) return "Error: cannot get current directory";

  std::filesystem::path full_path = cwd / path;
  full_path = std::filesystem::weakly_canonical(full_path, ec);
  if (ec) return "Error: invalid path";

  // Security: ensure the resolved path is still within current working directory
  std::filesystem::path normalized_cwd = std::filesystem::weakly_canonical(cwd, ec);
  if (ec) return "Error: cannot resolve current directory";

  auto cwd_str = normalized_cwd.string();
  auto target_str = full_path.string();
  if (target_str.find(cwd_str) != 0) {
    return "Error: path outside current directory (traversal not allowed)";
  }

  std::ofstream file(full_path);
  if (!file.is_open()) return "Error: cannot write to " + path;
  file << content;
  return "Successfully wrote " + std::to_string(content.size()) + " bytes to " + path;
}

}  // namespace pu::tools
