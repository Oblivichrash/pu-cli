// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/ask_dir_tool.hpp"

#include "pu/core/json.hpp"
#include "pu/runtime/runtime.hpp"

#include <boost/json.hpp>

#include <exception>
#include <memory>
#include <utility>

namespace pu::tools {

namespace {

// The environment variable a nested turn inherits. The depth is carried this way rather than
// through the agent table because a command the model runs starts a fresh `pu`, and only the
// environment crosses that boundary by itself.
constexpr const char* kDepthEnv = "PU_ASK_DEPTH";

}  // namespace

AskDirTool::AskDirTool(std::filesystem::path root, std::size_t depth)
    : root_(std::move(root)), depth_(depth) {}

std::string AskDirTool::Name() const { return "ask_dir"; }

std::string AskDirTool::Description() const {
  return "Ask a question in another directory, which reads its own files and answers in one "
         "go. You get the answer and not the reading: use it for what that directory knows "
         "and you would have to read half of it to find out.";
}

boost::json::value AskDirTool::ParametersSchema() const {
  return boost::json::object{
      {"type", "object"},
      {"properties",
       boost::json::object{
           {"dir",
            boost::json::object{
                {"type", "string"},
                {"description",
                 "The directory to ask, relative to this one. It must have its own .pu/ with "
                 "an agents.json naming a default_agent."}}},
           {"prompt",
            boost::json::object{
                {"type", "string"},
                {"description",
                 "The whole question. The other directory has no idea what this conversation "
                 "has said, so state what it needs rather than referring to it."}}},
       }},
      {"required", boost::json::array{"dir", "prompt"}},
  };
}

std::string AskDirTool::Execute(const boost::json::value& args, pu::ToolContext& ctx) {
  const std::string dir = json::ValueOrDefault<std::string>(args, "dir", "");
  const std::string prompt = json::ValueOrDefault<std::string>(args, "prompt", "");

  if (dir.empty() || prompt.empty()) {
    return MakeRefusal("A directory and a prompt are both required", true);
  }

  // A turn that arrived from elsewhere is already spending someone else's budget. Asking
  // further would make one request fan out with no one left to attribute the cost to.
  if (!ctx.peer_origin.empty()) {
    return MakeRefusal("a relayed turn cannot ask another directory", true);
  }

  // A path that climbs out of the tree would let a prompt reach any `.pu` on the machine,
  // including the asking one's, which is the same conversation this turn is meant to leave.
  const std::filesystem::path relative(dir);
  if (relative.is_absolute()) {
    return MakeRefusal("a directory to ask is relative to this one, not absolute", true);
  }

  std::error_code ec;
  std::filesystem::path target = std::filesystem::weakly_canonical(root_ / relative, ec);
  if (ec) {
    return MakeRefusal("cannot resolve the directory '" + dir + "': " + ec.message());
  }

  const std::filesystem::path base = std::filesystem::weakly_canonical(root_, ec);
  if (!ec) {
    auto beside = base.begin();
    auto wanted = target.begin();
    for (; beside != base.end() && wanted != target.end(); ++beside, ++wanted) {
      if (*beside != *wanted) break;
    }
    if (beside != base.end()) {
      return MakeRefusal("'" + dir + "' leads outside the directory this agent may read", true);
    }
  }
  if (target == base) {
    return MakeRefusal("'" + dir + "' is this directory; ask the question here instead", true);
  }

  const std::filesystem::path table = target / ".pu" / "agents.json";
  if (!std::filesystem::exists(table, ec)) {
    return MakeRefusal("'" + dir + "' has no .pu/agents.json, so it configures no model");
  }

  try {
    pu::Runtime runtime;
    runtime.SetWorkspaceRoot(target);
    // Ephemeral on both sides: the other directory's stored conversation is not read, and
    // nothing this turn learns is written down where its next run would inherit it.
    runtime.SetEphemeral();
    runtime.Initialize();

    // What the nested turn sees about its depth, for a `pu` it may start from a tool.
    const char* previous_depth = std::getenv(kDepthEnv);
    const std::string carried = std::to_string(depth_ + 1);
    setenv(kDepthEnv, carried.c_str(), 1);

    bool is_command = false;
    const ExecutionResult result = runtime.ProcessInput(prompt, is_command);
    runtime.Shutdown();

    if (previous_depth != nullptr) {
      setenv(kDepthEnv, previous_depth, 1);
    } else {
      unsetenv(kDepthEnv);
    }

    if (result.has_error) {
      return MakeRefusal("the turn in '" + dir + "' failed: " + result.error_message);
    }
    if (result.content.empty()) {
      return MakeRefusal("'" + dir + "' answered with nothing");
    }
    return result.content;
  } catch (const std::exception& e) {
    return MakeRefusal("could not ask '" + dir + "': " + e.what());
  }
}

}  // namespace pu::tools
