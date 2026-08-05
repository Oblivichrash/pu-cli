// SPDX-License-Identifier: GPL-3.0-only
#include "pu/executor.hpp"
#include "pu/tools/builtin_tools.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

TEST_CASE("BuildStaticSystemContext includes environment info",
          "[executor]") {
  Executor executor(nullptr);
  std::string msg = executor.BuildStaticSystemContext();

  REQUIRE(msg.find("=== Environment ===") != std::string::npos);
  REQUIRE(msg.find("OS: ") != std::string::npos);
  REQUIRE(msg.find("Kernel: ") != std::string::npos);
}

TEST_CASE("BuildStaticSystemContext includes security policy when set",
          "[executor]") {
  Executor executor(nullptr);
  config::SecurityPolicy policy;
  policy.sandbox_root = "/tmp/sandbox";
  policy.forbidden_patterns = {"rm -rf", "sudo"};
  executor.SetSecurityPolicy(policy);

  std::string msg = executor.BuildStaticSystemContext();

  REQUIRE(msg.find("=== Security Policy ===") != std::string::npos);
  REQUIRE(msg.find("Sandbox root: /tmp/sandbox") != std::string::npos);
  REQUIRE(msg.find("Forbidden patterns: ") != std::string::npos);
  REQUIRE(msg.find("'rm -rf'") != std::string::npos);
  REQUIRE(msg.find("'sudo'") != std::string::npos);
}

TEST_CASE("BuildStaticSystemContext shows empty forbidden patterns correctly",
          "[executor]") {
  Executor executor(nullptr);
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  std::string msg = executor.BuildStaticSystemContext();

  REQUIRE(msg.find("Forbidden patterns: (none)") != std::string::npos);
}

TEST_CASE(
    "BuildStaticSystemContext shows no-security-policy message when unset",
    "[executor]") {
  Executor executor(nullptr);
  std::string msg = executor.BuildStaticSystemContext();

  REQUIRE(msg.find("(no security policy set)") != std::string::npos);
}

TEST_CASE("BuildStaticSystemContext includes working directory section",
          "[executor]") {
  Executor executor(nullptr);
  config::SecurityPolicy policy;
  policy.sandbox_root = "/home/user/project";
  executor.SetSecurityPolicy(policy);

  std::string msg = executor.BuildStaticSystemContext();

  REQUIRE(msg.find("=== Working Directory ===") != std::string::npos);
  REQUIRE(msg.find("/home/user/project") != std::string::npos);
}

TEST_CASE("BuildStaticSystemContext working directory defaults to dot",
          "[executor]") {
  Executor executor(nullptr);
  std::string msg = executor.BuildStaticSystemContext();

  REQUIRE(msg.find("=== Working Directory ===") != std::string::npos);
  REQUIRE(msg.find(".\n") != std::string::npos);
}

TEST_CASE("BuildStaticSystemContext includes tool use guidelines",
          "[executor]") {
  Executor executor(nullptr);
  std::string msg = executor.BuildStaticSystemContext();

  REQUIRE(msg.find("=== Tool Use Guidelines ===") != std::string::npos);
  REQUIRE(msg.find("step-by-step plan") != std::string::npos);
  REQUIRE(msg.find("head -n 50") != std::string::npos);
  REQUIRE(msg.find("ask_user") != std::string::npos);
  REQUIRE(msg.find("parallel tool calls") != std::string::npos);
}

TEST_CASE("ProbeStaticEnvironment runs once and caches OS/kernel info",
          "[executor]") {
  Executor executor(nullptr);
  const auto& info = executor.GetStaticEnvInfo();
  REQUIRE(info.probed);
  REQUIRE(!info.os_name.empty());
  REQUIRE(!info.kernel_version.empty());
}

namespace {

class MockLLM : public LLMProvider {
 public:
  explicit MockLLM(std::vector<ToolCall> calls, std::string content = "")
      : calls_(std::move(calls)), content_(std::move(content)) {}

  ChatResult Chat(const std::vector<ChatMessage>& /*history*/,
                  const std::vector<ToolDefinition>& /*tools*/,
                  std::function<void(const std::string&)> /*content_callback*/,
                  std::function<void(const ToolCall&)> tool_callback) override {
    ChatResult r;
    r.content = content_;
    for (const auto& c : calls_) {
      if (tool_callback) tool_callback(c);
    }
    return r;
  }

  bool SupportsTools() const override { return true; }
  std::string GetModelName() const override { return "mock"; }

 private:
  std::vector<ToolCall> calls_;
  std::string content_;
};

class TrackingTool : public Tool {
 public:
  std::string Name() const override { return "tracking_tool"; }
  std::string Description() const override { return "records execution"; }
  std::string ParametersSchema() const override {
    return R"({"type":"object"})";
  }
  std::string Execute(const nlohmann::json& /*args*/,
                      ToolContext& /*ctx*/) override {
    ++executions;
    return R"({"success":true,"stdout":"ran","stderr":"","error":"","exit_code":0})";
  }

  int executions = 0;
};

}  // namespace

TEST_CASE("Executor returns ask_user question without running other tools",
          "[executor][tool_loop]") {
  Toolbox toolbox;
  toolbox.RegisterTool(std::make_unique<tools::AskUserTool>());
  auto tracking = std::make_unique<TrackingTool>();
  auto* tracking_ptr = tracking.get();
  toolbox.RegisterTool(std::move(tracking));

  Executor executor(&toolbox);
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  executor.SetSecurityPolicy(policy);

  ToolCall call;
  call.id = "call_ask_1";
  call.name = "ask_user";
  call.arguments["question"] = "Should I overwrite the existing file?";

  MockLLM mock({call}, "thinking out loud");
  Workspace ws;
  ExecutionResult result = executor.Execute("help me", ws, &mock);

  REQUIRE(result.content == "Should I overwrite the existing file?");
  REQUIRE(result.was_streamed == false);
  REQUIRE(result.has_error == false);
  REQUIRE(tracking_ptr->executions == 0);
}
