// SPDX-License-Identifier: GPL-3.0-only

#include "experts/bash/bash_expert.hpp"
#include "executor/command_executor.hpp"
#include "backends/ollama/ollama_backend.hpp"
#include "backends/ollama/ollama_token_adapter.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include "pu/expert.hpp"
#include "pu/expert_config.hpp"
#include "pu/http/http_client.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>

using namespace pu::backend;
using namespace pu::backends::ollama;
using namespace pu::experts;
using namespace pu::executor;
using namespace pu::config;
using namespace pu::tests;
using namespace std::string_literals;

namespace {

class FakeExecutor : public CommandExecutor {
 public:
  using CommandExecutor::CommandExecutor;

  ExecutionResult Execute(const std::string& command) override {
    ExecutionResult res;
    auto risk = AssessRisk(command);
    if (risk.level == RiskLevel::kDangerous) {
      res.was_intercepted = true;
      res.intercept_reason = risk.reason;
      return res;
    }
    res.exit_code = 0;
    res.stdout_content = "fake output";
    return res;
  }
};

// Helper to send a string as a chunk through the WriteCallback
inline void FeedLine(pu::http::WriteCallback& cb, const std::string& line) {
  cb(const_cast<char*>(line.data()), line.size());
}

auto MakeBackend(MockHttpClient*& mock_out) {
  auto mock = std::make_unique<MockHttpClient>();
  mock_out = mock.get();
  auto adapter = std::make_unique<OllamaTokenAdapter>();
  OllamaBackend::Config cfg;
  cfg.model = "test";
  cfg.host = "http://localhost";
  return std::make_unique<OllamaBackend>(std::move(cfg), std::move(mock), std::move(adapter));
}

}  // namespace

TEST_CASE("BashExpert safe command auto-approve", "[integration][bash]") {
  MockHttpClient* mock = nullptr;
  auto backend = MakeBackend(mock);
  auto executor = std::make_unique<FakeExecutor>(".");
  BashExpert expert("bash", std::move(backend), std::move(executor),
                    ConfirmationPolicy::kAutoSafe);

  int call_count = 0;
  mock->simulate_response = [&](const std::string&, const std::string&,
                                const std::vector<std::string>&,
                                pu::http::WriteCallback cb,
                                std::error_code& ec) {
    ec.clear();
    if (call_count == 0) {
      auto msg = R"({"message":{"content":"Running ls","tool_calls":[{"id":"1","function":{"name":"execute_bash","arguments":{"command":"ls"}}}]}})"s + "\n";
      FeedLine(cb, msg);
    } else {
      FeedLine(cb, R"({"message":{"content":"fake output"}})"s + "\n");
      FeedLine(cb, R"({"done":true})"s + "\n");
    }
    ++call_count;
  };

  pu::expert::ExpertContext ctx;
  bool asked = false;
  ctx.request_confirmation = [&](const pu::expert::ConfirmationRequest&) {
    asked = true;
    return pu::expert::ConfirmationChoice::kDeny;
  };
  ctx.show_reasoning = false;

  auto result = expert.Handle("list files", ctx);
  REQUIRE_FALSE(asked);
  REQUIRE(result.find("fake output") != std::string::npos);
}

TEST_CASE("BashExpert dangerous command blocked", "[integration][bash]") {
  MockHttpClient* mock = nullptr;
  auto backend = MakeBackend(mock);
  auto executor = std::make_unique<FakeExecutor>(".");
  BashExpert expert("bash", std::move(backend), std::move(executor),
                    ConfirmationPolicy::kAlwaysAsk);

  int call_count = 0;
  mock->simulate_response = [&](const std::string&, const std::string&,
                                const std::vector<std::string>&,
                                pu::http::WriteCallback cb,
                                std::error_code& ec) {
    ec.clear();
    if (call_count == 0) {
      auto msg = R"({"message":{"content":"Dangerous","tool_calls":[{"id":"1","function":{"name":"execute_bash","arguments":{"command":"rm -rf /"}}}]}})"s + "\n";
      FeedLine(cb, msg);
    } else {
      FeedLine(cb, R"({"message":{"content":"Blocked"}})"s + "\n");
      FeedLine(cb, R"({"done":true})"s + "\n");
    }
    ++call_count;
  };

  pu::expert::ExpertContext ctx;
  bool asked = false;
  ctx.request_confirmation = [&](const pu::expert::ConfirmationRequest&) {
    asked = true;
    return pu::expert::ConfirmationChoice::kApproveOnce;
  };

  auto result = expert.Handle("remove all", ctx);
  REQUIRE(asked);
  REQUIRE(result.find("Blocked") != std::string::npos);
}

TEST_CASE("BashExpert neutral command confirmed", "[integration][bash]") {
  MockHttpClient* mock = nullptr;
  auto backend = MakeBackend(mock);
  auto executor = std::make_unique<FakeExecutor>(".");
  BashExpert expert("bash", std::move(backend), std::move(executor),
                    ConfirmationPolicy::kAlwaysAsk);

  int call_count = 0;
  mock->simulate_response = [&](const std::string&, const std::string&,
                                const std::vector<std::string>&,
                                pu::http::WriteCallback cb,
                                std::error_code& ec) {
    ec.clear();
    if (call_count == 0) {
      auto msg = R"({"message":{"content":"Searching","tool_calls":[{"id":"1","function":{"name":"execute_bash","arguments":{"command":"grep error log.txt"}}}]}})"s + "\n";
      FeedLine(cb, msg);
    } else {
      FeedLine(cb, R"({"message":{"content":"fake output"}})"s + "\n");
      FeedLine(cb, R"({"done":true})"s + "\n");
    }
    ++call_count;
  };

  pu::expert::ExpertContext ctx;
  bool asked = false;
  ctx.request_confirmation = [&](const pu::expert::ConfirmationRequest&) {
    asked = true;
    return pu::expert::ConfirmationChoice::kApproveOnce;
  };

  auto result = expert.Handle("search logs", ctx);
  REQUIRE(asked);
  REQUIRE(result.find("fake output") != std::string::npos);
}

TEST_CASE("BashExpert neutral command denied", "[integration][bash]") {
  MockHttpClient* mock = nullptr;
  auto backend = MakeBackend(mock);
  auto executor = std::make_unique<FakeExecutor>(".");
  BashExpert expert("bash", std::move(backend), std::move(executor),
                    ConfirmationPolicy::kAlwaysAsk);

  int call_count = 0;
  mock->simulate_response = [&](const std::string&, const std::string&,
                                const std::vector<std::string>&,
                                pu::http::WriteCallback cb,
                                std::error_code& ec) {
    ec.clear();
    if (call_count == 0) {
      auto msg = R"({"message":{"content":"Will check","tool_calls":[{"id":"1","function":{"name":"execute_bash","arguments":{"command":"df -h"}}}]}})"s + "\n";
      FeedLine(cb, msg);
      ++call_count;
    }
  };

  pu::expert::ExpertContext ctx;
  ctx.request_confirmation = [](const pu::expert::ConfirmationRequest&) {
    return pu::expert::ConfirmationChoice::kDeny;
  };

  auto result = expert.Handle("disk usage", ctx);
  REQUIRE(result.find("cancelled") != std::string::npos);
}
