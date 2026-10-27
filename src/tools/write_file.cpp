// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/write_file.hpp"

#include "pu/core/json.hpp"

#include <filesystem>
#include <fstream>

namespace pu::tools {

namespace {

std::string Refuse(const std::string& error, bool blocked = false) {
  return MakeToolResultJson(false, "", error, -1, blocked);
}

// Compares paths by component rather than by string prefix: `/tmp/safe-evil` starts with
// `/tmp/safe` and is not inside it.
bool Within(const std::filesystem::path& root, const std::filesystem::path& target) {
  auto r = root.begin();
  auto t = target.begin();
  for (; r != root.end(); ++r, ++t) {
    if (t == target.end() || *r != *t) return false;
  }
  return true;
}

}  // namespace

std::string WriteFileTool::Name() const { return "write_file"; }

std::string WriteFileTool::Description() const { return "Write text to a file."; }

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
  const std::string path = json::ValueOrDefault<std::string>(args, "path", "");
  const std::string content = json::ValueOrDefault<std::string>(args, "content", "");
  if (path.empty()) return Refuse("'path' is required");
  if (ctx.security == nullptr) return Refuse("security policy not set");

  std::error_code ec;
  const auto root = std::filesystem::weakly_canonical(
      std::filesystem::path(ctx.security->sandbox_root), ec);
  if (ec) return Refuse("cannot resolve sandbox root: " + ctx.security->sandbox_root);

  const auto target = std::filesystem::weakly_canonical(root / path, ec);
  if (ec) return Refuse("invalid path");
  if (!Within(root, target)) {
    return Refuse("path outside sandbox root (traversal not allowed)", true);
  }

  std::filesystem::create_directories(target.parent_path(), ec);
  if (ec) return Refuse("cannot create parent directories");

  std::ofstream file(target);
  if (!file.is_open()) return Refuse("cannot write to " + path);
  file << content;

  return MakeToolResultJson(
      true, "Successfully wrote " + std::to_string(content.size()) + " bytes to " + path, "", 0);
}

}  // namespace pu::tools
