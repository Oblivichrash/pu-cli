// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/ask_dir_tool.hpp"

#include "pu/core/json.hpp"
#include "pu/core/process.hpp"
#include "pu/core/text.hpp"
#include "pu/paging/paging.hpp"
#include "pu/tools/delegation.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <utility>

namespace pu::tools {

namespace {

// How long a delegated turn may run in all. It is generous on purpose: a sub-agent that reads a
// repository and runs its tests is doing the work it was asked for, and a tight figure would turn
// slow into broken. It is a ceiling rather than a target, so the common turn never meets it.
constexpr std::chrono::milliseconds kDelegatedTurnBudget{std::chrono::minutes(10)};

std::string Listed(const std::vector<std::string>& names) { return text::Join(names); }

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

AskDirTool::AskDirTool(std::filesystem::path root, std::vector<std::string> allowed,
                       paging::Pager* pager, CancelToken cancel_token)
    : root_(std::move(root)),
      allowed_(std::move(allowed)),
      pager_(pager),
      cancel_token_(std::move(cancel_token)) {}

std::string AskDirTool::Name() const { return "ask_dir"; }

std::string AskDirTool::Description() const {
  // The directory branch is the one a model gets wrong, because nothing about a path says
  // it needs a table: it writes to the directory, asks, and reads the refusal as a bug. So
  // the shape is stated here, where the model reads it before the first call.
  return "Ask another agent a question and get only its answer. `ask` names who answers: an "
         "agent this table configures, or a directory whose own agent answers there. You get "
         "the conclusion and not the reading.\n"
         "A directory must already carry its own configuration before you ask it: write "
         "'<dir>/.pu/agents.json' with write_file first, holding {\"default_agent\": \"name\", "
         "\"agents\": [{\"name\": \"name\", \"backend\": {\"gateway\", \"model\"\")]} "
         "(the same shape as this workspace's own .pu/agents.json). It is not created for "
         "you, and a directory without one is refused rather than answered.";
}

boost::json::value AskDirTool::ParametersSchema() const {
  std::string ask_description;
  if (allowed_.empty()) {
    ask_description =
        "A directory to ask in, relative to this one, for what it knows about its own files. "
        "That directory must already have .pu/agents.json -- write it with write_file before "
        "asking; nothing creates it for you.";
  } else {
    ask_description = std::string("Who answers: one of ") + Listed(allowed_) +
                      ", or a directory to ask in, relative to this one, for what it knows "
                      "about its own files. That directory must already have .pu/agents.json "
                      "-- write it with write_file before asking; nothing creates it for you.";
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

// The answer as a receipt: the conclusion, plus where it came from. It is not a page -- there
// is no window to read and no cursor to hand back -- but it carries the one field the
// projection reasons about, `buffer.source`. That is what makes two answers to one question
// two runs of one source, and therefore lets the older answer be elided instead of re-sent on
// every turn. Without it the answer went in as bare prose, no `OriginOf` could read it, and it
// stayed in the request in full for as long as the conversation lasted.
std::string AskDirTool::Answer(const std::string& target, const std::string& prompt,
                               const DelegationResult& result, pu::ToolContext& ctx) {
  Charge(ctx, result);

  if (pager_ == nullptr) return result.answer;

  // The question is the identity, and the target is part of the question: `review` asked of
  // two directories asks two different things. The separator is the one `BashTool` uses for
  // the same reason -- a key that can be spelled two ways answers as two questions.
  std::string key = target;
  key.push_back('\x1f');
  key.append(prompt);

  core::Verdict verdict;
  verdict.command = "ask_dir " + target;
  verdict.ok = true;

  const auto answer = pager_->Record(result.answer, std::move(verdict), std::move(key));
  boost::json::object extra{
      {"buffer",
       boost::json::object{{"id", answer->id()},
                           {"source", answer->source()},
                           {"rank", answer->rank()}}},
  };
  return MakeToolResultJson(true, result.answer, "", 0, false, Refusal::kNone, &extra);
}

std::string AskDirTool::Execute(const boost::json::value& args, pu::ToolContext& ctx) {
  (void)ctx;
  const std::string ask = json::ValueOrDefault<std::string>(args, "ask", "");
  const std::string prompt = json::ValueOrDefault<std::string>(args, "prompt", "");

  if (prompt.empty()) return MakeRefusal("A question is required", true);
  if (ask.empty()) return MakeRefusal("Name who answers: an agent or a directory", true);

  Delegation request;
  request.prompt = prompt;
  // The turn that is waiting is this one's turn: a stop pressed while the delegate is thinking
  // has to reach the thinking, or the button does nothing for as long as the question is slow.
  request.cancel_token = cancel_token_;
  // A ceiling on the whole delegated turn. The transport's own idle deadline bounds a single
  // read; a turn may make many, and each one may be a tool running for minutes, so without a
  // figure here the only stop is the process.
  request.timeout = kDelegatedTurnBudget;

  // One field names who answers, and which of the two it is is read rather than declared: a
  // name this agent may reach is an agent, and anything else is a directory to look for. The
  // list is what settles it, so a name that is also a directory is asked as the agent — the
  // narrower reading, and the one the table wrote down on purpose.
  const auto named = std::find(allowed_.begin(), allowed_.end(), ask);
  if (named != allowed_.end()) {
    request.agent = ask;
    request.dir = root_;
    const DelegationResult result = RunDelegatedTurn(request);
    if (!result.error.empty()) return MakeRefusal(result.error, true);
    return Answer(request.dir.string(), prompt, result, ctx);
  }

  // A path climbing out of the tree would let one prompt reach any `.pu` on the machine.
  const std::filesystem::path relative(ask);
  if (relative.is_absolute()) {
    return MakeRefusal(
        "a directory to ask is relative to this one, not absolute: drop the leading '" +
            relative.root_path().string() + "' and give the path from here, or name the " +
            "directory from a workspace that can already see it",
        true);
  }

  std::error_code ec;
  const std::filesystem::path target = std::filesystem::weakly_canonical(root_ / relative, ec);
  if (ec) {
    return MakeRefusal("cannot resolve the directory '" + ask + "': " + ec.message());
  }
  const std::filesystem::path base = std::filesystem::weakly_canonical(root_, ec);
  if (!ec && !Inside(base, target)) {
    // Named as a directory that is out of reach, not as a mistake in the spelling: the
    // difference is that the asker may still want that directory, and the only way in is a
    // workspace that sits there.
    return MakeRefusal("'" + ask +
                           "' is outside this directory's own tree, so it is not a directory "
                           "this agent may ask. What is reachable is a path under here, or an "
                           "agent this table's `delegates` names",
                       true);
  }
  if (target == base) {
    return MakeRefusal("'" + ask + "' is this directory; ask the question here instead", true);
  }
  request.dir = target;

  // The turn runs a whole runtime of its own, which is what reads the files; the runtime is
  // the one that builds it, so this tool asks for the turn rather than starting one.
  const DelegationResult result = RunDelegatedTurn(request);
  if (!result.error.empty()) return MakeRefusal(result.error, true);
  return Answer(target.string(), prompt, result, ctx);
}

}  // namespace pu::tools
