// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <filesystem>
#include <string>

#include "pu/tools/ask_dir_tool.hpp"
#include "pu/tools/tool.hpp"

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
  pu::tools::AskDirTool tool(fs::current_path(), Allowed{"reader", "reviewer"});
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

TEST_CASE("An agent that may ask nobody is offered only a directory", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), Allowed{});
  const boost::json::value schema = tool.ParametersSchema();
  const boost::json::object& properties = schema.at("properties").as_object();

  REQUIRE(properties.contains("ask"));
  // Nothing to name means no name is offered: the page says "a directory", so a model that
  // reads it does not spend a call on an agent it could never reach.
  REQUIRE_FALSE(Says(boost::json::serialize(properties.at("ask")), "one of"));
  REQUIRE(schema.at("required").as_array().size() == 2);
}

TEST_CASE("ask_dir reads a name it may reach as an agent, not a directory", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;

  // No such directory exists, so a directory reading would refuse to resolve it. Reaching the
  // table instead is what shows which of the two readings the name was given.
  const std::string reply = tool.Execute(Ask("reader", "hi"), ctx);
  REQUIRE_FALSE(Says(reply, "cannot resolve the directory"));
  REQUIRE_FALSE(Says(reply, "relative to this one"));
}

TEST_CASE("ask_dir refuses an agent it was not told it may ask", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;

  const std::string reply =
      tool.Execute(boost::json::object{{"ask", "stranger"}, {"prompt", "hi"}}, ctx);

  // A name that is not on the list is read as a directory, and a directory that does not
  // exist is not an agent either — the refusal has to leave the caller able to tell.
  REQUIRE(Says(reply, "stranger"));
}

TEST_CASE("ask_dir refuses to ask a named agent when it may ask none", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), Allowed{});
  pu::ToolContext ctx;

  const std::string reply =
      tool.Execute(boost::json::object{{"ask", "reader"}, {"prompt", "hi"}}, ctx);

  REQUIRE(Says(reply, "reader"));
}


TEST_CASE("ask_dir adds what the turn it asked cost to the asking turn's tally", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), Allowed{});
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
  pu::tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;

  const std::string reply = tool.Execute(boost::json::object{{"prompt", "hi"}}, ctx);
  REQUIRE(Says(reply, "Name who answers"));
}

TEST_CASE("ask_dir refuses a directory that configures no agent", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;

  const std::string reply = tool.Execute(Ask("ghost-directory-that-is-not-here", "hi"), ctx);
  REQUIRE(Says(reply, "no .pu/agents.json"));
}

TEST_CASE("ask_dir will not be pointed at an absolute path", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;

  const std::string reply = tool.Execute(Ask("/etc", "hi"), ctx);
  REQUIRE(Says(reply, "relative"));
}

TEST_CASE("ask_dir will not climb out of the directory it was given", "[ask_dir]") {
  const fs::path root = fs::current_path() / "some" / "nested" / "place";
  pu::tools::AskDirTool tool(root, Allowed{"reader"});
  pu::ToolContext ctx;

  const std::string reply = tool.Execute(Ask("../../../..", "hi"), ctx);
  REQUIRE(Says(reply, "outside"));
}

TEST_CASE("ask_dir refuses to ask the directory it is already in", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;

  const std::string reply = tool.Execute(Ask(".", "hi"), ctx);
  REQUIRE(Says(reply, "ask the question here"));
}

TEST_CASE("ask_dir refuses an empty question", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;

  REQUIRE(Says(tool.Execute(Ask("somewhere", ""), ctx), "required"));
  REQUIRE(Says(tool.Execute(Ask("", ""), ctx), "required"));
}
