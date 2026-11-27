// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <filesystem>
#include <string>

#include "pu/agent_tools/ask_dir_tool.hpp"
#include "pu/agent_tools/delegation.hpp"
#include "pu/agent_tools/tool.hpp"
#include "tests/mocks/test_helpers.hpp"

namespace {

namespace fs = std::filesystem;

// A refusal is the tool's own vocabulary for "I will not run this", and every path that ends
// without a provider is a refusal rather than an answer. Matching on the words a reader sees
// keeps the test about what the model is told, which is the thing it acts on.
bool Says(const std::string& reply, const std::string& words) {
  return reply.find(words) != std::string::npos;
}

using Args = boost::json::object;
using Allowed = std::vector<std::string>;

// One field names who answers — an agent this table configures, or a directory — and the
// prompt is the whole question.
boost::json::value Ask(const std::string& who, const std::string& prompt) {
  return boost::json::object{{"ask", who}, {"prompt", prompt}};
}

}  // namespace

TEST_CASE("ask_dir names itself as a tool the model can see", "[ask_dir]") {
  pu::agent_tools::AskDirTool tool(fs::current_path(), Allowed{"reader", "reviewer"});
  REQUIRE(tool.Name() == "ask_dir");
  REQUIRE(Says(tool.Description(), "only its answer"));

  const boost::json::value schema = tool.ParametersSchema();
  const boost::json::object& properties = schema.at("properties").as_object();
  REQUIRE(properties.contains("ask"));
  REQUIRE(properties.contains("prompt"));
  // One field names who answers, so there is no pair to keep in step, and both are needed:
  // a question with nobody to put it to is not a call.
  REQUIRE_FALSE(properties.contains("agent"));
  REQUIRE_FALSE(properties.contains("dir"));
  REQUIRE(schema.at("required").as_array().size() == 2);

  // The agents it may reach are in the page, so the model reads them rather than guessing and
  // being refused.
  REQUIRE(Says(boost::json::serialize(properties.at("ask")), "reader, reviewer"));
}

TEST_CASE("The page says a directory needs its own table before it is asked", "[ask_dir]") {
  // The directory branch is the one a model gets wrong, and no amount of a good refusal
  // repairs a first call it could have made right. Both ways of reaching the page say it.
  pu::agent_tools::AskDirTool reached(fs::current_path(), Allowed{"reader"});
  pu::agent_tools::AskDirTool alone(fs::current_path(), Allowed{});

  REQUIRE(Says(reached.Description(), "write_file"));
  REQUIRE(Says(reached.Description(), "refused rather than answered"));
  for (const auto* tool : {&reached, &alone}) {
    const boost::json::value schema = tool->ParametersSchema();
    const std::string ask = boost::json::serialize(schema.at("properties").at("ask"));
    REQUIRE(Says(ask, ".pu/agents.json"));
    REQUIRE(Says(ask, "nothing creates it for you"));
  }
}

TEST_CASE("An agent that may ask nobody is offered only a directory", "[ask_dir]") {
  pu::agent_tools::AskDirTool tool(fs::current_path(), Allowed{});
  const boost::json::value schema = tool.ParametersSchema();
  const boost::json::object& properties = schema.at("properties").as_object();

  REQUIRE(properties.contains("ask"));
  // Nothing to name means no name is offered: the page says "a directory", so a model that
  // reads it does not spend a call on an agent it could never reach.
  REQUIRE_FALSE(Says(boost::json::serialize(properties.at("ask")), "one of"));
  REQUIRE(schema.at("required").as_array().size() == 2);
}

TEST_CASE("ask_dir reads a name it may reach as an agent, not a directory", "[ask_dir]") {
  pu::agent_tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;

  // No such directory exists, so a directory reading would refuse to resolve it. Reaching the
  // table instead is what shows which of the two readings the name was given.
  const std::string reply = tool.Execute(Ask("reader", "hi"), ctx);
  REQUIRE_FALSE(Says(reply, "cannot resolve the directory"));
  REQUIRE_FALSE(Says(reply, "relative to this one"));
}

TEST_CASE("ask_dir refuses an agent it was not told it may ask", "[ask_dir]") {
  pu::agent_tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;

  const std::string reply =
      tool.Execute(boost::json::object{{"ask", "stranger"}, {"prompt", "hi"}}, ctx);

  // A name that is not on the list is read as a directory, and a directory that does not
  // exist is not an agent either — the refusal has to leave the caller able to tell.
  REQUIRE(Says(reply, "stranger"));
}

TEST_CASE("ask_dir refuses to ask a named agent when it may ask none", "[ask_dir]") {
  pu::agent_tools::AskDirTool tool(fs::current_path(), Allowed{});
  pu::ToolContext ctx;

  const std::string reply =
      tool.Execute(boost::json::object{{"ask", "reader"}, {"prompt", "hi"}}, ctx);

  REQUIRE(Says(reply, "reader"));
}


TEST_CASE("ask_dir adds what the turn it asked cost to the asking turn's tally", "[ask_dir]") {
  pu::agent_tools::AskDirTool tool(fs::current_path(), Allowed{});
  pu::ToolContext ctx;
  pu::ToolContext::Spending spent;
  ctx.spent = &spent;

  // A call that never reaches a provider spends nothing, and must not invent a figure: the
  // tally is what a receipt prints, and a made-up one is worse than a missing one.
  const std::string refused = tool.Execute(Ask("ghost-directory-that-is-not-here", "hi"), ctx);
  REQUIRE(Says(refused, "no .pu/agents.json"));
  CHECK(spent.prompt_tokens == 0);
  CHECK(spent.completion_tokens == 0);
  CHECK_FALSE(spent.cost.has_value());
  CHECK(spent.turns == 0);
}

TEST_CASE("ask_dir refuses a call naming neither an agent nor a directory", "[ask_dir]") {
  pu::agent_tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;

  const std::string reply = tool.Execute(boost::json::object{{"prompt", "hi"}}, ctx);
  REQUIRE(Says(reply, "Name who answers"));
}

TEST_CASE("ask_dir refuses a directory that configures no agent", "[ask_dir]") {
  pu::agent_tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;

  const std::string reply = tool.Execute(Ask("ghost-directory-that-is-not-here", "hi"), ctx);
  REQUIRE(Says(reply, "no .pu/agents.json"));
  // A refusal a model cannot act on is one it retries. The remedy rides with the reason: the
  // file to write, and the shape to write in, are both in the message.
  REQUIRE(Says(reply, "write_file"));
  REQUIRE(Says(reply, "then ask again"));
}

TEST_CASE("ask_dir will not be pointed at an absolute path", "[ask_dir]") {
  pu::agent_tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;

  // An absolute path that is absolute on every platform. '/etc' is not one on Windows, where
  // a leading '/' is a drive-relative root, so this case would pass by taking `/etc` as a
  // relative name and failing somewhere else entirely.
  const std::string absolute = fs::current_path().root_path().string();
  REQUIRE(fs::path(absolute).is_absolute());

  const std::string reply = tool.Execute(Ask(absolute, "hi"), ctx);
  REQUIRE(Says(reply, "relative"));
  // Which part to drop, rather than which rule was broken: the caller keeps the path it
  // meant and rewrites the one thing that is wrong with it.
  REQUIRE(Says(reply, "leading '"));
}

TEST_CASE("ask_dir will not climb out of the directory it was given", "[ask_dir]") {
  const fs::path root = fs::current_path() / "some" / "nested" / "place";
  pu::agent_tools::AskDirTool tool(root, Allowed{"reader"});
  pu::ToolContext ctx;

  const std::string reply = tool.Execute(Ask("../../../..", "hi"), ctx);
  REQUIRE(Says(reply, "outside"));
  // The boundary is named along with the two ways that stay inside it, because "outside"
  // alone reads as a mistake in the spelling of a path the caller still wants.
  REQUIRE(Says(reply, "under here"));
  REQUIRE(Says(reply, "delegates"));
}

TEST_CASE("ask_dir refuses to ask the directory it is already in", "[ask_dir]") {
  pu::agent_tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;

  const std::string reply = tool.Execute(Ask(".", "hi"), ctx);
  REQUIRE(Says(reply, "ask the question here"));
}

TEST_CASE("ask_dir refuses an empty question", "[ask_dir]") {
  pu::agent_tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;

  REQUIRE(Says(tool.Execute(Ask("somewhere", ""), ctx), "required"));
  REQUIRE(Says(tool.Execute(Ask("", ""), ctx), "required"));
}

TEST_CASE("ask_dir refuses a directory this question is already being asked in", "[ask_dir]") {
  pu::agent_tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;
  // The chain is read before anything is looked for, so the target needs no table: the refusal
  // below is the chain's, and a case that had to build one first would be testing the table.
  ctx.delegated_visited.push_back(fs::current_path() / "elsewhere");

  const std::string reply = tool.Execute(Ask("elsewhere", "hi"), ctx);
  REQUIRE(Says(reply, "already answering"));
  REQUIRE(Says(reply, "further up the chain"));
}

TEST_CASE("ask_dir looks at the chain before it decides a directory has no table", "[ask_dir]") {
  pu::agent_tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;
  ctx.delegated_visited.push_back(fs::current_path() / "elsewhere");

  // A cycle check that matches too eagerly answers "already answering" here and never looks at
  // the target at all. The chain names a different place, so this one is refused for the reason
  // it would have been refused with no chain: it has no table.
  const std::string reply = tool.Execute(Ask("another-place", "hi"), ctx);
  REQUIRE(Says(reply, "no .pu/agents.json"));
  REQUIRE_FALSE(Says(reply, "already answering"));
}

// The rule itself, stated without a runtime or a provider: the tool cases above reach it
// through an executor that publishes a chain, and those check that the chain arrives. These
// check that the chain, once it arrives, is read correctly.

TEST_CASE("An empty chain refuses nothing", "[delegation]") {
  // The top of a chain has nothing behind it, so there is nothing to re-enter. A rule that
  // refused on an empty chain would make the first delegation of every run impossible.
  REQUIRE(pu::RefusedReEntry(fs::current_path(), {}).empty());
}

TEST_CASE("A chain refuses the workspace it already names", "[delegation]") {
  const fs::path here = fs::current_path();
  const std::string refused = pu::RefusedReEntry(here, {here});
  REQUIRE_FALSE(refused.empty());
  // The message names the directory, so a model told to stop can tell which one to avoid.
  REQUIRE(refused.find(here.string()) != std::string::npos);
  REQUIRE(refused.find("further up the chain") != std::string::npos);
}

TEST_CASE("A chain compares places, not spellings", "[delegation]") {
  // The same directory reaches here by different routes: an absolute path, one with `..` in
  // it, and whatever the shell happened to produce. A rule that compared strings would let a
  // cycle through on the second spelling, which is the whole failure it exists to stop.
  //
  // The `..` leg has to exist for the comparison to be about the rule rather than about
  // `weakly_canonical`'s tolerance for missing parents, so this case builds one -- in a temp
  // directory of its own, because a case that writes into the checkout leaves something
  // behind when it fails.
  const fs::path root = pu::tests::UniqueTempPath("pu_reentry");
  const fs::path deep = root / "sub";
  fs::create_directories(deep);

  REQUIRE_FALSE(pu::RefusedReEntry(root, {deep / ".."}).empty());
  // And the same comparison the other way round: the chain names the deep path, the target is
  // its parent. A rule that only normalised one side would pass the line above and fail this.
  REQUIRE_FALSE(pu::RefusedReEntry(root, {deep / ".." / "."}).empty());

  fs::remove_all(root);
}

TEST_CASE("A chain lets through a directory it does not name", "[delegation]") {
  // A guard that refused everything would satisfy the cases above and break delegation
  // altogether, so the accepting side is stated too.
  const fs::path here = fs::current_path();
  REQUIRE(pu::RefusedReEntry(here / "elsewhere", {here / "another"}).empty());
}

TEST_CASE("AsOnePlace leaves a path it cannot resolve as it found it", "[delegation]") {
  // `weakly_canonical` fails on the filesystem for reasons that say nothing about whether the
  // place is a cycle. Returning the input is the answer that refuses no call, which is the
  // safe direction to fail in.
  const fs::path odd = fs::current_path() / "a" / ".." / "b";
  REQUIRE(pu::AsOnePlace(odd) == fs::weakly_canonical(odd));
  REQUIRE(pu::AsOnePlace({}).empty());
}
