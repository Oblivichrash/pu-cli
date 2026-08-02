// SPDX-License-Identifier: GPL-3.0-only
#include "pu/executor/executor.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <string>

using namespace pu;

TEST_CASE("ExtractToolResultContent parses success JSON and returns stdout",
          "[executor]") {
  nlohmann::json j;
  j["success"] = true;
  j["stdout"] = "hello world";
  j["stderr"] = "";
  j["error"] = "";
  j["exit_code"] = 0;

  std::string result = Executor::ExtractToolResultContent(j.dump());
  REQUIRE(result == "hello world");
}

TEST_CASE(
    "ExtractToolResultContent parses failure JSON and returns error field",
    "[executor]") {
  nlohmann::json j;
  j["success"] = false;
  j["stdout"] = "";
  j["stderr"] = "some stderr";
  j["error"] = "Command failed (exit 1)";
  j["exit_code"] = 1;

  std::string result = Executor::ExtractToolResultContent(j.dump());
  REQUIRE(result == "Command failed (exit 1)");
}

TEST_CASE(
    "ExtractToolResultContent returns raw string for non-JSON input",
    "[executor]") {
  std::string raw = "plain text output";
  std::string result = Executor::ExtractToolResultContent(raw);
  REQUIRE(result == raw);
}

TEST_CASE(
    "ExtractToolResultContent returns raw string for JSON without success key",
    "[executor]") {
  nlohmann::json j;
  j["other"] = "data";

  std::string result = Executor::ExtractToolResultContent(j.dump());
  REQUIRE(result == j.dump());
}

TEST_CASE(
    "ExtractToolResultContent returns raw string for JSON array",
    "[executor]") {
  nlohmann::json j = nlohmann::json::array({"a", "b"});

  std::string result = Executor::ExtractToolResultContent(j.dump());
  REQUIRE(result == j.dump());
}

TEST_CASE("BuildSystemContextMessage includes environment info when probed",
          "[executor]") {
  Executor executor(nullptr);
  // ProbeStaticEnvironment is called in constructor; OS/kernel/tools are
  // populated from the live system, so we only verify structural presence.
  pu::Workspace ws;
  std::string msg = executor.BuildSystemContextMessage(ws);

  REQUIRE(msg.find("=== Environment ===") != std::string::npos);
  REQUIRE(msg.find("OS: ") != std::string::npos);
  REQUIRE(msg.find("Kernel: ") != std::string::npos);
  REQUIRE(msg.find("Available tools: ") != std::string::npos);
}

TEST_CASE("BuildSystemContextMessage includes security policy when set",
          "[executor]") {
  Executor executor(nullptr);
  config::SecurityPolicy policy;
  policy.sandbox_root = "/tmp/sandbox";
  policy.forbidden_patterns = {"rm -rf", "sudo"};
  executor.SetSecurityPolicy(policy);

  pu::Workspace ws;
  std::string msg = executor.BuildSystemContextMessage(ws);

  REQUIRE(msg.find("=== Security Policy ===") != std::string::npos);
  REQUIRE(msg.find("Sandbox root: /tmp/sandbox") != std::string::npos);
  REQUIRE(msg.find("Forbidden patterns: ") != std::string::npos);
  REQUIRE(msg.find("'rm -rf'") != std::string::npos);
  REQUIRE(msg.find("'sudo'") != std::string::npos);
}

TEST_CASE("BuildSystemContextMessage shows empty forbidden patterns correctly",
          "[executor]") {
  Executor executor(nullptr);
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  pu::Workspace ws;
  std::string msg = executor.BuildSystemContextMessage(ws);

  REQUIRE(msg.find("Forbidden patterns: (none)") != std::string::npos);
}

TEST_CASE(
    "BuildSystemContextMessage shows no-security-policy message when unset",
    "[executor]") {
  Executor executor(nullptr);
  pu::Workspace ws;
  std::string msg = executor.BuildSystemContextMessage(ws);

  REQUIRE(msg.find("(no security policy set)") != std::string::npos);
}

TEST_CASE("BuildSystemContextMessage includes working directory section",
          "[executor]") {
  Executor executor(nullptr);
  config::SecurityPolicy policy;
  policy.sandbox_root = "/home/user/project";
  executor.SetSecurityPolicy(policy);

  pu::Workspace ws;
  std::string msg = executor.BuildSystemContextMessage(ws);

  REQUIRE(msg.find("=== Working Directory ===") != std::string::npos);
  REQUIRE(msg.find("/home/user/project") != std::string::npos);
}

TEST_CASE("BuildSystemContextMessage working directory defaults to dot",
          "[executor]") {
  Executor executor(nullptr);
  pu::Workspace ws;
  std::string msg = executor.BuildSystemContextMessage(ws);

  REQUIRE(msg.find("=== Working Directory ===") != std::string::npos);
  // Without a security policy, the working directory section shows ".".
  REQUIRE(msg.find(".\n") != std::string::npos);
}

TEST_CASE("BuildSystemContextMessage includes file artifacts section",
          "[executor]") {
  Executor executor(nullptr);
  pu::Workspace ws;
  pu::Artifact a;
  a.type = pu::Artifact::kFilePath;
  a.content = "/tmp/test.txt";
  ws.AddArtifact(a);

  std::string msg = executor.BuildSystemContextMessage(ws);

  REQUIRE(msg.find("=== Known File Paths ===") != std::string::npos);
  REQUIRE(msg.find("/tmp/test.txt") != std::string::npos);
}

TEST_CASE("BuildSystemContextMessage shows none for empty file artifacts",
          "[executor]") {
  Executor executor(nullptr);
  pu::Workspace ws;
  std::string msg = executor.BuildSystemContextMessage(ws);

  REQUIRE(msg.find("=== Known File Paths ===") != std::string::npos);
  REQUIRE(msg.find("(none)") != std::string::npos);
}

TEST_CASE(
    "BuildSystemContextMessage truncates file artifacts beyond 8 entries",
    "[executor]") {
  Executor executor(nullptr);
  pu::Workspace ws;
  // Add 12 file artifacts; only the last 8 should appear.
  for (int i = 0; i < 12; ++i) {
    pu::Artifact fp;
    fp.type = pu::Artifact::kFilePath;
    fp.content = "/tmp/file_" + std::to_string(i);
    ws.AddArtifact(fp);
  }

  std::string msg = executor.BuildSystemContextMessage(ws);

  // First 4 (indices 0-3) should be missing.
  REQUIRE(msg.find("/tmp/file_0") == std::string::npos);
  REQUIRE(msg.find("/tmp/file_1") == std::string::npos);
  REQUIRE(msg.find("/tmp/file_2") == std::string::npos);
  REQUIRE(msg.find("/tmp/file_3") == std::string::npos);
  // Last 8 (indices 4-11) should be present.
  REQUIRE(msg.find("/tmp/file_4") != std::string::npos);
  REQUIRE(msg.find("/tmp/file_11") != std::string::npos);
}

TEST_CASE(
    "BuildSystemContextMessage includes recent tool executions section",
    "[executor]") {
  Executor executor(nullptr);
  pu::Workspace ws;

  // Simulate a successful tool execution in history.
  ChatMessage tool_msg;
  tool_msg.role = "tool";
  tool_msg.tool_name = "execute_bash";
  nlohmann::json success_j;
  success_j["success"] = true;
  success_j["stdout"] = "hello";
  success_j["error"] = "";
  tool_msg.content = success_j.dump();
  ws.Append(tool_msg);

  std::string msg = executor.BuildSystemContextMessage(ws);

  REQUIRE(msg.find("=== Recent Tool Executions ===") != std::string::npos);
  REQUIRE(msg.find("[execute_bash] OK:") != std::string::npos);
  REQUIRE(msg.find("hello") != std::string::npos);
}

TEST_CASE(
    "BuildSystemContextMessage shows FAILED status for failed tool executions",
    "[executor]") {
  Executor executor(nullptr);
  pu::Workspace ws;

  ChatMessage tool_msg;
  tool_msg.role = "tool";
  tool_msg.tool_name = "execute_bash";
  nlohmann::json fail;
  fail["success"] = false;
  fail["stdout"] = "";
  fail["error"] = "command not found";
  tool_msg.content = fail.dump();
  ws.Append(tool_msg);

  std::string msg = executor.BuildSystemContextMessage(ws);

  REQUIRE(msg.find("- [execute_bash] FAILED:") != std::string::npos);
  REQUIRE(msg.find("command not found") != std::string::npos);
  REQUIRE(msg.find(
      "hint: check the error and retry with a corrected command") !=
          std::string::npos);
}

TEST_CASE(
    "BuildSystemContextMessage handles non-JSON tool results gracefully",
    "[executor]") {
  Executor executor(nullptr);
  pu::Workspace ws;

  ChatMessage tool_msg;
  tool_msg.role = "tool";
  tool_msg.tool_name = "mcp.files.read";
  tool_msg.content = "raw text result";
  ws.Append(tool_msg);

  std::string msg = executor.BuildSystemContextMessage(ws);

  REQUIRE(msg.find("- [mcp.files.read] ") != std::string::npos);
  REQUIRE(msg.find("raw text result") != std::string::npos);
}

TEST_CASE("BuildSystemContextMessage shows none when no tool executions exist",
          "[executor]") {
  Executor executor(nullptr);
  pu::Workspace ws;
  std::string msg = executor.BuildSystemContextMessage(ws);

  REQUIRE(msg.find("=== Recent Tool Executions ===") != std::string::npos);
  REQUIRE(msg.find("(none)") != std::string::npos);
}

TEST_CASE("ProbeStaticEnvironment runs once and caches", "[executor]") {
  // The constructor calls ProbeStaticEnvironment.
  Executor executor(nullptr);

  // The second call is a no-op because probed is already true.
  // We confirm available_tools contains expected entries.
  const auto& info = executor.GetStaticEnvInfo();
  REQUIRE(info.probed);
  REQUIRE(!info.os_name.empty());
  REQUIRE(!info.kernel_version.empty());

  // At least some tools should be detected on a developer machine.
  bool has_bash = false;
  bool has_python3 = false;
  for (const auto& t : info.available_tools) {
    if (t == "bash") has_bash = true;
    if (t == "python3") has_python3 = true;
  }
  // On a typical Linux system, at least one of bash/python3 is available.
  REQUIRE((has_bash || has_python3));
}

TEST_CASE("ProbeStaticEnvironment available_tools excludes missing tools",
          "[executor]") {
  Executor executor(nullptr);
  const auto& info = executor.GetStaticEnvInfo();

  // Tools like "nonexistent_tool_xyz" should never appear.
  for (const auto& t : info.available_tools) {
    REQUIRE(t != "nonexistent_tool_xyz");
  }
}