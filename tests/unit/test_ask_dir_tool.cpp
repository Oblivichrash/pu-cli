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

boost::json::value Ask(const std::string& dir, const std::string& prompt) {
  return boost::json::object{{"dir", dir}, {"prompt", prompt}};
}

}  // namespace

TEST_CASE("ask_dir names itself as a tool the model can see", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), Allowed{"reader", "reviewer"});
  REQUIRE(tool.Name() == "ask_dir");
  REQUIRE(Says(tool.Description(), "only its answer"));

  const boost::json::value schema = tool.ParametersSchema();
  const boost::json::object& properties = schema.at("properties").as_object();
  REQUIRE(properties.contains("dir"));
  REQUIRE(properties.contains("prompt"));
  REQUIRE(properties.contains("agent"));
  // A prompt is the one thing every call needs; where it runs and who answers have defaults.
  REQUIRE(schema.at("required").as_array().size() == 1);

  // The agents it may reach are in the page, so the model reads them rather than guessing and
  // being refused.
  REQUIRE(Says(boost::json::serialize(properties.at("agent")), "reader, reviewer"));
}

TEST_CASE("An agent that may ask nobody is offered no agent to name", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), Allowed{});
  const boost::json::value schema = tool.ParametersSchema();
  const boost::json::object& properties = schema.at("properties").as_object();

  REQUIRE_FALSE(properties.contains("agent"));
  REQUIRE(schema.at("required").as_array().size() == 2);
}

TEST_CASE("ask_dir refuses an agent it was not told it may ask", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;

  const std::string reply =
      tool.Execute(boost::json::object{{"agent", "stranger"}, {"prompt", "hi"}}, ctx);

  REQUIRE(Says(reply, "may not ask 'stranger'"));
  REQUIRE(Says(reply, "it may ask: reader"));
}

TEST_CASE("ask_dir refuses to ask a named agent when it may ask none", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), Allowed{});
  pu::ToolContext ctx;

  const std::string reply =
      tool.Execute(boost::json::object{{"agent", "reader"}, {"prompt", "hi"}}, ctx);

  REQUIRE(Says(reply, "may not ask 'reader'"));
}

TEST_CASE("ask_dir refuses a call naming neither an agent nor a directory", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), Allowed{"reader"});
  pu::ToolContext ctx;

  const std::string reply = tool.Execute(boost::json::object{{"prompt", "hi"}}, ctx);
  REQUIRE(Says(reply, "Name an agent or a directory"));
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
