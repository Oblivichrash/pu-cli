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

boost::json::value Args(const std::string& dir, const std::string& prompt) {
  return boost::json::object{{"dir", dir}, {"prompt", prompt}};
}

}  // namespace

TEST_CASE("ask_dir names itself as a tool the model can see", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), 0);
  REQUIRE(tool.Name() == "ask_dir");
  REQUIRE(Says(tool.Description(), "another directory"));

  const boost::json::value schema = tool.ParametersSchema();
  const boost::json::object& properties = schema.at("properties").as_object();
  REQUIRE(properties.contains("dir"));
  REQUIRE(properties.contains("prompt"));
  REQUIRE(schema.at("required").as_array().size() == 2);
}

TEST_CASE("ask_dir refuses a directory it cannot reach", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), 0);
  pu::ToolContext ctx;

  const std::string reply = tool.Execute(Args("ghost-directory-that-is-not-here", "hi"), ctx);
  REQUIRE(Says(reply, "no .pu/agents.json"));
}

TEST_CASE("ask_dir will not be pointed at an absolute path", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), 0);
  pu::ToolContext ctx;

  const std::string reply = tool.Execute(Args("/etc", "hi"), ctx);
  REQUIRE(Says(reply, "relative"));
}

TEST_CASE("ask_dir will not climb out of the directory it was given", "[ask_dir]") {
  const fs::path root = fs::current_path() / "some" / "nested" / "place";
  pu::tools::AskDirTool tool(root, 0);
  pu::ToolContext ctx;

  const std::string reply = tool.Execute(Args("../../../..", "hi"), ctx);
  REQUIRE(Says(reply, "outside"));
}

TEST_CASE("ask_dir refuses to ask the directory it is already in", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), 0);
  pu::ToolContext ctx;

  const std::string reply = tool.Execute(Args(".", "hi"), ctx);
  REQUIRE(Says(reply, "ask the question here"));
}

TEST_CASE("ask_dir refuses an empty question", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), 0);
  pu::ToolContext ctx;

  REQUIRE(Says(tool.Execute(Args("somewhere", ""), ctx), "required"));
  REQUIRE(Says(tool.Execute(Args("", "a question"), ctx), "required"));
}

TEST_CASE("ask_dir stops a turn that is itself an answer from asking further", "[ask_dir]") {
  pu::tools::AskDirTool tool(fs::current_path(), 1);
  pu::ToolContext ctx;

  const std::string reply = tool.Execute(Args("somewhere", "hi"), ctx);
  REQUIRE(Says(reply, "further"));
}
