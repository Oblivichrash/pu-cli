// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/ask_dir_tool.hpp"

#include "pu/core/json.hpp"
#include "pu/runtime/runtime.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <exception>
#include <memory>
#include <utility>

namespace pu::tools {

namespace {

std::string Listed(const std::vector<std::string>& names) {
  std::string out;
  for (const std::string& name : names) out += (out.empty() ? "" : ", ") + name;
  return out;
}

// Whether `target` is `base` or sits under it. Compared a component at a time: a string prefix
// would call `/a/pu-cli-old` a directory under `/a/pu-cli`, and a tool whose whole job is to
// stay inside a boundary cannot be wrong about where the boundary is.
bool Inside(const std::filesystem::path& base, const std::filesystem::path& target) {
  auto beside = base.begin();
  auto wanted = target.begin();
  for (; beside != base.end() && wanted != target.end(); ++beside, ++wanted) {
    if (*beside != *wanted) return false;
  }
  return beside == base.end();
}

}  // namespace

AskDirTool::AskDirTool(std::filesystem::path root, std::vector<std::string> allowed)
    : root_(std::move(root)), allowed_(std::move(allowed)) {}

std::string AskDirTool::Name() const { return "ask_dir"; }

std::string AskDirTool::Description() const {
  return "Ask another agent a question and get only its answer. Name one it configures, and it "
         "answers in this directory; name a directory, and that directory's own agent answers "
         "there. You get the conclusion and not the reading.";
}

boost::json::value AskDirTool::ParametersSchema() const {
  boost::json::object properties{
      {"agent",
       boost::json::object{
           {"type", "string"},
           {"description",
            "The agent to ask, by name. Empty asks the directory's default agent."}}},
      {"dir",
       boost::json::object{
           {"type", "string"},
           {"description",
            "A directory to ask in, relative to this one, for what it knows about its own "
            "files. Needs its own .pu/ with an agents.json."}}},
      {"prompt",
       boost::json::object{
           {"type", "string"},
           {"description",
            "The whole question. The agent asked has no idea what this conversation has said, "
            "so state what it needs rather than referring to it."}}},
  };

  if (allowed_.empty()) {
    properties.erase("agent");
    return boost::json::object{
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", boost::json::array{"dir", "prompt"}},
    };
  }

  boost::json::object agent = properties.at("agent").as_object();
  agent["description"] =
      std::string("The agent to ask, by name. One of: ") + Listed(allowed_) +
      ". Empty asks the directory's own default agent.";
  properties["agent"] = std::move(agent);

  return boost::json::object{
      {"type", "object"},
      {"properties", std::move(properties)},
      {"required", boost::json::array{"prompt"}},
  };
}

std::string AskDirTool::Execute(const boost::json::value& args, pu::ToolContext& ctx) {
  (void)ctx;
  const std::string dir = json::ValueOrDefault<std::string>(args, "dir", "");
  const std::string agent = json::ValueOrDefault<std::string>(args, "agent", "");
  const std::string prompt = json::ValueOrDefault<std::string>(args, "prompt", "");

  if (prompt.empty()) return MakeRefusal("A question is required", true);

  Delegation request;
  request.prompt = prompt;
  request.agent = agent;

  if (dir.empty()) {
    // No directory: the question is for another agent this table configures, and it runs here.
    if (agent.empty()) {
      return MakeRefusal("Name an agent or a directory to ask", true);
    }
    if (std::find(allowed_.begin(), allowed_.end(), agent) == allowed_.end()) {
      return MakeRefusal("this agent may not ask '" + agent + "'; it may ask: " + Listed(allowed_),
                         true);
    }
    request.dir = root_;
  } else {
    // A path climbing out of the tree would let one prompt reach any `.pu` on the machine.
    const std::filesystem::path relative(dir);
    if (relative.is_absolute()) {
      return MakeRefusal("a directory to ask is relative to this one, not absolute", true);
    }

    std::error_code ec;
    const std::filesystem::path target = std::filesystem::weakly_canonical(root_ / relative, ec);
    if (ec) {
      return MakeRefusal("cannot resolve the directory '" + dir + "': " + ec.message());
    }
    const std::filesystem::path base = std::filesystem::weakly_canonical(root_, ec);
    if (!ec && !Inside(base, target)) {
      return MakeRefusal("'" + dir + "' leads outside the directory this agent may read", true);
    }
    if (target == base) {
      return MakeRefusal("'" + dir + "' is this directory; ask the question here instead", true);
    }
    request.dir = target;

    // The directory answers with its own agent unless one is named, and a named one is held to
    // the same list: otherwise a directory would be a way round the table that named it.
    if (!agent.empty() &&
        std::find(allowed_.begin(), allowed_.end(), agent) == allowed_.end()) {
      return MakeRefusal("this agent may not ask '" + agent + "'; it may ask: " + Listed(allowed_),
                         true);
    }
  }

  Runtime runtime;  // The turn runs a whole runtime of its own, which is what reads the files.
  const DelegationResult result = runtime.Delegate(request);
  if (!result.error.empty()) return MakeRefusal(result.error, true);
  return result.answer;
}

}  // namespace pu::tools
