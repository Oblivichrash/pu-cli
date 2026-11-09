// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/write_file.hpp"

#include "pu/core/json.hpp"

#include <filesystem>
#include <fstream>

namespace pu::tools {

namespace {

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

std::string WriteFileTool::Description() const {
  // Nothing about "write a file" says that a dot-directory is in scope, and the one folder a
  // model is most likely to be asked to create is a `.pu/` for a sub-agent. Saying so here
  // is cheaper than the turn spent discovering it, and the sandbox sentence is the half of
  // the answer a model otherwise learns by being refused outside it.
  return "Write text to a file. The path is relative to this workspace's sandbox root, and "
         "the parent directories are created. A path that resolves outside the sandbox is "
         "refused; dot-directories such as '.pu/' are ordinary paths and are written like "
         "any other.";
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
  const std::string path = json::ValueOrDefault<std::string>(args, "path", "");
  const std::string content = json::ValueOrDefault<std::string>(args, "content", "");
  if (path.empty()) return MakeRefusal("'path' is required");
  if (ctx.security == nullptr) return MakeRefusal("security policy not set");

  std::error_code ec;
  const auto root = std::filesystem::weakly_canonical(
      std::filesystem::path(ctx.security->sandbox_root), ec);
  if (ec) return MakeRefusal("cannot resolve sandbox root: " + ctx.security->sandbox_root);

  const auto target = std::filesystem::weakly_canonical(root / path, ec);
  if (ec) return MakeRefusal("invalid path");
  if (!Within(root, target)) {
    // The root is named because it is what explains the refusal: a path the model computed
    // from the directory it can see is out of bounds only relative to a root it never saw.
    return MakeRefusal("path outside sandbox root (traversal not allowed): paths are relative "
                       "to '" + root.string() + "', which is this agent's sandbox",
                       true);
  }

  std::filesystem::create_directories(target.parent_path(), ec);
  if (ec) return MakeRefusal("cannot create parent directories");

  std::ofstream file(target);
  if (!file.is_open()) return MakeRefusal("cannot write to " + path);
  file << content;

  return MakeToolResultJson(
      true, "Successfully wrote " + std::to_string(content.size()) + " bytes to " + path, "", 0);
}

}  // namespace pu::tools
