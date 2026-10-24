// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/builtin_tools.hpp"
#include "pu/tools/tool.hpp"
#include "tests/mocks/test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace pu;
using namespace pu::tools;
using namespace pu::tests;

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream f(path);
  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return content;
}

}  // namespace

TEST_CASE("execute_bash returns success JSON on successful command", "[builtin_tools]") {
  ExecuteBashToolStandard tool(".");
  pu::ToolContext ctx;

  boost::json::value args = boost::json::object{};
  args.as_object()["command"] = "echo hello";
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == true);
  REQUIRE(boost::json::value_to<std::string>(j.at("output")).find("hello") != std::string::npos);
  REQUIRE(j.at("blocked") == false);
  REQUIRE(j.at("error") == "");
  REQUIRE(j.at("exit_code") == 0);
}

TEST_CASE("execute_bash returns failure JSON on non-zero exit code", "[builtin_tools]") {
  ExecuteBashToolStandard tool(".");
  pu::ToolContext ctx;

  boost::json::value args = boost::json::object{};
  args.as_object()["command"] = "nonexistent_command_pu_test";
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("Command failed") !=
          std::string::npos);
  REQUIRE(j.at("exit_code") != 0);
}

TEST_CASE("Execute_bash returns error JSON for missing command parameter", "[builtin_tools]") {
  ExecuteBashToolStandard tool(".");

  boost::json::value args = boost::json::object{};
  pu::ToolContext ctx;
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("output") == "");
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
  REQUIRE(boost::json::value_to<std::string>(j.at("error"))
              .find("forbidden pattern") != std::string::npos);
}

TEST_CASE("ExecuteBash blocks dangerous commands via risk assessment", "[builtin_tools]") {
  ExecuteBashToolStandard tool(".");

  pu::ToolContext ctx;
  boost::json::value args = boost::json::object{};
  args.as_object()["command"] = "rm -rf /";
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("Command rejected:") !=
          std::string::npos);
  REQUIRE(j.at("blocked") == true);
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
  REQUIRE(
      boost::json::value_to<std::string>(j.at("error")).find("exceeds maximum allowed length") !=
      std::string::npos);
}

TEST_CASE("Write_file returns success JSON on successful write", "[builtin_tools]") {
  std::filesystem::path tmpdir = UniqueTempPath("pu_write_test");
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
  REQUIRE(boost::json::value_to<std::string>(j.at("output")).find("Successfully wrote") !=
          std::string::npos);
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

TEST_CASE("Write_file returns error JSON for path traversal attempt", "[builtin_tools]") {
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

TEST_CASE("Write_file returns error JSON when no security policy set", "[builtin_tools]") {
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

TEST_CASE("A refusal says which kind it is", "[tools][refusal]") {
  // What a policy refusal looks like: the flag alone, since the command is what to rework.
  const pu::tools::ToolResult policy = pu::tools::ParseToolResult(
      R"({"success":false,"blocked":true,"output":"","error":"contains forbidden pattern","exit_code":-1})");
  REQUIRE(policy.valid);
  REQUIRE(policy.blocked);
  REQUIRE(policy.refusal == pu::tools::Refusal::kPolicy);

  // What an agent with approval_patterns and no channel to ask on looks like. Same flag,
  // different remedy: nothing is wrong with the command.
  const pu::tools::ToolResult unaskable = pu::tools::ParseToolResult(pu::tools::MakeToolResultJson(
      false, "", "needs approval and no channel is open", -1, true,
      pu::tools::Refusal::kNobodyToAsk));
  REQUIRE(unaskable.valid);
  REQUIRE(unaskable.blocked);
  REQUIRE(unaskable.refusal == pu::tools::Refusal::kNobodyToAsk);

  // A result that ran carries no refusal at all, and one that failed on its own is not a
  // refusal either.
  REQUIRE(pu::tools::ParseToolResult(
              R"({"success":true,"blocked":false,"output":"a.txt","error":"","exit_code":0})")
              .refusal == pu::tools::Refusal::kNone);
  REQUIRE(pu::tools::ParseToolResult(
              R"({"success":false,"blocked":false,"output":"","error":"exit 1","exit_code":1})")
              .refusal == pu::tools::Refusal::kNone);
}
