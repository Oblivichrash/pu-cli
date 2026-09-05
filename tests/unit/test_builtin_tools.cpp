// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/builtin_tools.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace pu;
using namespace pu::tools;

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream f(path);
  std::string content((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
  return content;
}

}  // namespace

TEST_CASE("execute_bash returns success JSON on successful command",
          "[builtin_tools]") {
  ExecuteBashToolStandard tool(".");
  pu::ToolContext ctx;

  boost::json::value args = boost::json::object{};
  args.as_object()["command"] = "echo hello";
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == true);
  REQUIRE(boost::json::value_to<std::string>(j.at("stdout")).find("hello") != std::string::npos);
  REQUIRE(j.at("stderr") == "");
  REQUIRE(j.at("error") == "");
  REQUIRE(j.at("exit_code") == 0);
}

TEST_CASE("execute_bash returns failure JSON on non-zero exit code",
          "[builtin_tools]") {
  ExecuteBashToolStandard tool(".");
  pu::ToolContext ctx;

  boost::json::value args = boost::json::object{};
  args.as_object()["command"] = "nonexistent_command_pu_test";
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("Command failed") != std::string::npos);
  REQUIRE(j.at("exit_code") != 0);
}

TEST_CASE("Execute_bash returns error JSON for missing command parameter",
          "[builtin_tools]") {
  ExecuteBashToolStandard tool(".");

  boost::json::value args = boost::json::object{};
  pu::ToolContext ctx;
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("stdout") == "");
  REQUIRE(j.at("error") == "'command' parameter is required");
  REQUIRE(j.at("exit_code") == -1);
}

TEST_CASE("Execute_bash blocks forbidden patterns", "[builtin_tools]") {
  ExecuteBashToolStandard tool(".");

  pu::ToolContext ctx;
  config::SecurityPolicy policy;
  policy.forbidden_patterns = {"rm -rf", "sudo"};
  ctx.security = &policy;

  boost::json::value args = boost::json::object{};
  args.as_object()["command"] = "sudo rm -rf /";
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find(
              "command contains forbidden pattern") != std::string::npos);
}

TEST_CASE("ExecuteBash blocks dangerous commands via risk assessment",
          "[builtin_tools]") {
  ExecuteBashToolStandard tool(".");

  pu::ToolContext ctx;
  boost::json::value args = boost::json::object{};
  args.as_object()["command"] = "rm -rf /";
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("Blocked:") != std::string::npos);
}

TEST_CASE("Execute_bash enforces max_command_length", "[builtin_tools]") {
  ExecuteBashToolStandard tool(".");

  pu::ToolContext ctx;
  config::SecurityPolicy policy;
  policy.max_command_length = 10;
  ctx.security = &policy;

  boost::json::value args = boost::json::object{};
  args.as_object()["command"] = "echo this is way too long for the limit";
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("exceeds maximum allowed length") !=
          std::string::npos);
}

TEST_CASE("Write_file returns success JSON on successful write",
          "[builtin_tools]") {
  std::filesystem::path tmpdir =
      std::filesystem::temp_directory_path() / "pu_write_test";
  std::filesystem::create_directories(tmpdir);

  WriteFileTool tool;
  pu::ToolContext ctx;
  config::SecurityPolicy policy;
  policy.sandbox_root = tmpdir.string();
  ctx.security = &policy;

  boost::json::value args = boost::json::object{};
  args.as_object()["path"] = "hello.txt";
  args.as_object()["content"] = "Hello, world!";
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == true);
  REQUIRE(boost::json::value_to<std::string>(j.at("stdout")).find("Successfully wrote") != std::string::npos);
  REQUIRE(j.at("error") == "");
  REQUIRE(j.at("exit_code") == 0);

  std::string written = ReadFile((tmpdir / "hello.txt").string());
  REQUIRE(written == "Hello, world!");

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("Write_file returns error JSON for missing path", "[builtin_tools]") {
  WriteFileTool tool;
  pu::ToolContext ctx;
  config::SecurityPolicy policy;
  policy.sandbox_root = "/tmp";
  ctx.security = &policy;

  boost::json::value args = boost::json::object{};
  args.as_object()["content"] = "some content";
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("error") == "'path' is required");
  REQUIRE(j.at("exit_code") == -1);
}

TEST_CASE("Write_file returns error JSON for path traversal attempt",
          "[builtin_tools]") {
  WriteFileTool tool;
  pu::ToolContext ctx;
  config::SecurityPolicy policy;
  policy.sandbox_root = "/tmp/safe";
  ctx.security = &policy;

  boost::json::value args = boost::json::object{};
  args.as_object()["path"] = "../etc/passwd";
  args.as_object()["content"] = "evil";
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("traversal") != std::string::npos);
}

TEST_CASE("Write_file returns error JSON when no security policy set",
          "[builtin_tools]") {
  WriteFileTool tool;
  pu::ToolContext ctx;

  boost::json::value args = boost::json::object{};
  args.as_object()["path"] = "test.txt";
  args.as_object()["content"] = "data";
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("error") == "security policy not set");
}

TEST_CASE("Ask_user returns clarification JSON with question", "[builtin_tools]") {
  AskUserTool tool;
  pu::ToolContext ctx;

  boost::json::value args = boost::json::object{};
  args.as_object()["question"] = "Which directory should I target?";
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("error") == "clarification_needed");
  REQUIRE(j.at("question") == "Which directory should I target?");
}

TEST_CASE("Ask_user returns empty question when argument missing",
          "[builtin_tools]") {
  AskUserTool tool;
  pu::ToolContext ctx;

  boost::json::value args = boost::json::object{};
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("error") == "clarification_needed");
  REQUIRE(j.at("question") == "");
}

TEST_CASE("Ask_user metadata exposes name, description, and schema",
          "[builtin_tools]") {
  AskUserTool tool;

  REQUIRE(tool.Name() == "ask_user");
  REQUIRE(tool.Description() == "Ask user for clarification.");

  auto schema = boost::json::parse(tool.ParametersSchema());
  REQUIRE(schema.at("type") == "object");
  REQUIRE(schema.at("required") == boost::json::value(boost::json::array{"question"}));
  REQUIRE(schema.at("properties").at("question").at("type") == "string");
}
