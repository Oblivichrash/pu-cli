// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/builtin_tools.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

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

  nlohmann::json args;
  args["command"] = "echo hello";
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j["success"] == true);
  REQUIRE(j["stdout"].get<std::string>().find("hello") != std::string::npos);
  REQUIRE(j["stderr"] == "");
  REQUIRE(j["error"] == "");
  REQUIRE(j["exit_code"] == 0);
}

TEST_CASE("execute_bash returns failure JSON on non-zero exit code",
          "[builtin_tools]") {
  ExecuteBashToolStandard tool(".");
  pu::ToolContext ctx;

  nlohmann::json args;
  args["command"] = "nonexistent_command_pu_test";
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j["success"] == false);
  REQUIRE(j["error"].get<std::string>().find("Command failed") != std::string::npos);
  REQUIRE(j["exit_code"] != 0);
}

TEST_CASE("Execute_bash returns error JSON for missing command parameter",
          "[builtin_tools]") {
  ExecuteBashToolStandard tool(".");

  nlohmann::json args;
  pu::ToolContext ctx;
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j["success"] == false);
  REQUIRE(j["stdout"] == "");
  REQUIRE(j["error"] == "'command' parameter is required");
  REQUIRE(j["exit_code"] == -1);
}

TEST_CASE("Execute_bash blocks forbidden patterns", "[builtin_tools]") {
  ExecuteBashToolStandard tool(".");

  pu::ToolContext ctx;
  config::SecurityPolicy policy;
  policy.forbidden_patterns = {"rm -rf", "sudo"};
  ctx.security = &policy;

  nlohmann::json args;
  args["command"] = "sudo rm -rf /";
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j["success"] == false);
  REQUIRE(j["error"].get<std::string>().find(
              "command contains forbidden pattern") != std::string::npos);
}

TEST_CASE("ExecuteBash blocks dangerous commands via risk assessment",
          "[builtin_tools]") {
  ExecuteBashToolStandard tool(".");

  pu::ToolContext ctx;
  nlohmann::json args;
  args["command"] = "rm -rf /";
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j["success"] == false);
  REQUIRE(j["error"].get<std::string>().find("Blocked:") != std::string::npos);
}

TEST_CASE("Execute_bash enforces max_command_length", "[builtin_tools]") {
  ExecuteBashToolStandard tool(".");

  pu::ToolContext ctx;
  config::SecurityPolicy policy;
  policy.max_command_length = 10;
  ctx.security = &policy;

  nlohmann::json args;
  args["command"] = "echo this is way too long for the limit";
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j["success"] == false);
  REQUIRE(j["error"].get<std::string>().find("exceeds maximum allowed length") !=
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

  nlohmann::json args;
  args["path"] = "hello.txt";
  args["content"] = "Hello, world!";
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j["success"] == true);
  REQUIRE(j["stdout"].get<std::string>().find("Successfully wrote") != std::string::npos);
  REQUIRE(j["error"] == "");
  REQUIRE(j["exit_code"] == 0);

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

  nlohmann::json args;
  args["content"] = "some content";
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j["success"] == false);
  REQUIRE(j["error"] == "'path' is required");
  REQUIRE(j["exit_code"] == -1);
}

TEST_CASE("Write_file returns error JSON for path traversal attempt",
          "[builtin_tools]") {
  WriteFileTool tool;
  pu::ToolContext ctx;
  config::SecurityPolicy policy;
  policy.sandbox_root = "/tmp/safe";
  ctx.security = &policy;

  nlohmann::json args;
  args["path"] = "../etc/passwd";
  args["content"] = "evil";
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j["success"] == false);
  REQUIRE(j["error"].get<std::string>().find("traversal") != std::string::npos);
}

TEST_CASE("Write_file returns error JSON when no security policy set",
          "[builtin_tools]") {
  WriteFileTool tool;
  pu::ToolContext ctx;

  nlohmann::json args;
  args["path"] = "test.txt";
  args["content"] = "data";
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j["success"] == false);
  REQUIRE(j["error"] == "security policy not set");
}

TEST_CASE("Ask_user returns clarification JSON with question", "[builtin_tools]") {
  AskUserTool tool;
  pu::ToolContext ctx;

  nlohmann::json args;
  args["question"] = "Which directory should I target?";
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j["success"] == false);
  REQUIRE(j["error"] == "clarification_needed");
  REQUIRE(j["question"] == "Which directory should I target?");
}

TEST_CASE("Ask_user rejects missing or empty question",
          "[builtin_tools]") {
  AskUserTool tool;
  pu::ToolContext ctx;

  nlohmann::json args;
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j["success"] == false);
  REQUIRE(j["error"] == "ask_user requires a non-empty 'question' string");
}

TEST_CASE("Ask_user rejects non-string question", "[builtin_tools]") {
  AskUserTool tool;
  pu::ToolContext ctx;

  nlohmann::json args;
  args["question"] = 123;
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j["success"] == false);
  REQUIRE(j["error"] == "ask_user requires a non-empty 'question' string");
}

TEST_CASE("Ask_user metadata exposes name, description, and schema",
          "[builtin_tools]") {
  AskUserTool tool;

  REQUIRE(tool.Name() == "ask_user");
  REQUIRE(tool.Description() == "Ask user for clarification.");

  auto schema = nlohmann::json::parse(tool.ParametersSchema());
  REQUIRE(schema["type"] == "object");
  REQUIRE(schema["required"] == nlohmann::json::array({"question"}));
  REQUIRE(schema["properties"]["question"]["type"] == "string");
}
