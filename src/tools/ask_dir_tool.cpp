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

// Folds what the asking turn cost onto the asking turn's tally, so nothing here is answered
// without its price reaching the receipt that prints it.
namespace {

void Charge(pu::ToolContext& ctx, const DelegationResult& result) {
  if (ctx.spent == nullptr) return;
  // What it cost is the asking turn's business: the child's provider billed its own runtime,
  // and a receipt that said this call was free would be the one number nobody could check.
  ctx.spent->prompt_tokens += result.usage.prompt_tokens;
  ctx.spent->completion_tokens += result.usage.completion_tokens;
  ctx.spent->cached_tokens += result.usage.cached_tokens;
  ctx.spent->turns += result.usage.turns;
  // A price adds only within one unit: two products' units are not comparable, and a total
  // that mixed them would read as this call's price when it is only part of it.
  if (result.usage.cost) {
    if (!ctx.spent->cost) {
      ctx.spent->cost = result.usage.cost;
    } else if (ctx.spent->cost->unit == result.usage.cost->unit) {
      ctx.spent->cost->amount += result.usage.cost->amount;
    } else {
      ctx.spent->cost.reset();
    }
  }
}

}  // namespace

AskDirTool::AskDirTool(std::filesystem::path root, std::vector<std::string> allowed)
    : root_(std::move(root)), allowed_(std::move(allowed)) {}

std::string AskDirTool::Name() const { return "ask_dir"; }

std::string AskDirTool::Description() const {
  return "Ask another agent a question and get only its answer. `ask` names who answers: an "
         "agent this table configures, or a directory whose own agent answers there. You get "
         "the conclusion and not the reading.";
}

boost::json::value AskDirTool::ParametersSchema() const {
  std::string ask_description;
  if (allowed_.empty()) {
    ask_description =
        "A directory to ask in, relative to this one, for what it knows about its own files. "
        "Needs its own .pu/ with an agents.json.";
  } else {
    ask_description = std::string("Who answers: one of ") + Listed(allowed_) +
                      ", or a directory to ask in, relative to this one, for what it knows "
                      "about its own files (that directory needs its own .pu/).";
  }

  boost::json::object properties{
      {"ask",
       boost::json::object{
           {"type", "string"},
           {"description", std::move(ask_description)}}},
      {"prompt",
       boost::json::object{
           {"type", "string"},
           {"description",
            "The whole question. The agent asked has no idea what this conversation has said, "
            "so state what it needs rather than referring to it."}}},
  };

  return boost::json::object{
      {"type", "object"},
      {"properties", std::move(properties)},
      {"required", boost::json::array{"ask", "prompt"}},
  };
}

std::string AskDirTool::Execute(const boost::json::value& args, pu::ToolContext& ctx) {
  (void)ctx;
  const std::string ask = json::ValueOrDefault<std::string>(args, "ask", "");
  const std::string prompt = json::ValueOrDefault<std::string>(args, "prompt", "");

  if (prompt.empty()) return MakeRefusal("A question is required", true);
  if (ask.empty()) return MakeRefusal("Name who answers: an agent or a directory", true);

  Delegation request;
  request.prompt = prompt;

  // One field names who answers, and which of the two it is is read rather than declared: a
  // name this agent may reach is an agent, and anything else is a directory to look for. The
  // list is what settles it, so a name that is also a directory is asked as the agent — the
  // narrower reading, and the one the table wrote down on purpose.
  const auto named = std::find(allowed_.begin(), allowed_.end(), ask);
  if (named != allowed_.end()) {
    request.agent = ask;
    request.dir = root_;
    Runtime runtime;
    const DelegationResult result = runtime.Delegate(request);
    if (!result.error.empty()) return MakeRefusal(result.error, true);
    Charge(ctx, result);
    return result.answer;
  }

  // A path climbing out of the tree would let one prompt reach any `.pu` on the machine.
  const std::filesystem::path relative(ask);
  if (relative.is_absolute()) {
    return MakeRefusal("a directory to ask is relative to this one, not absolute", true);
  }

  std::error_code ec;
  const std::filesystem::path target = std::filesystem::weakly_canonical(root_ / relative, ec);
  if (ec) {
    return MakeRefusal("cannot resolve the directory '" + ask + "': " + ec.message());
  }
  const std::filesystem::path base = std::filesystem::weakly_canonical(root_, ec);
  if (!ec && !Inside(base, target)) {
    return MakeRefusal("'" + ask + "' leads outside the directory this agent may read", true);
  }
  if (target == base) {
    return MakeRefusal("'" + ask + "' is this directory; ask the question here instead", true);
  }
  request.dir = target;

  Runtime runtime;  // The turn runs a whole runtime of its own, which is what reads the files.
  const DelegationResult result = runtime.Delegate(request);
  if (!result.error.empty()) return MakeRefusal(result.error, true);
  Charge(ctx, result);
  return result.answer;
}

}  // namespace pu::tools
