// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "pu/config/agents.hpp"
#include "pu/runtime.hpp"
#include "pu/session/session.hpp"
#include "tests/mocks/test_helpers.hpp"

namespace fs = std::filesystem;
using namespace pu;
using namespace pu::tests;

namespace {

class BackendSourceFixture {
 public:
  BackendSourceFixture() {
    root_ = UniqueTempPath("pu_backend_source");
    std::error_code ec;
    fs::remove_all(root_, ec);
    fs::create_directories(root_ / ".pu");
    Write(0.3f);
  }

  ~BackendSourceFixture() {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  const fs::path& root() const { return root_; }

  void Write(float temperature) {
    boost::json::value cfg = {
        {"default_agent", "chat"},
        {"agents",
         boost::json::array{
             boost::json::value{
                 {"name", "chat"},
                 {"backend",
                  {
                      {"gateway", "openai"},
                      {"host", "http://127.0.0.1:1"},
                      {"model", "test-model"},
                      {"temperature", temperature},
                      {"thinking", "none"},
                  }},
             },
             boost::json::value{
                 {"name", "coder"},
                 {"backend",
                  {
                      {"gateway", "openai"},
                      {"host", "http://127.0.0.1:1"},
                      {"model", "coder-model"},
                      {"temperature", 0.4},
                  }},
             },
         }},
    };
    std::ofstream out(root_ / ".pu" / "agents.json", std::ios::trunc);
    out << boost::json::serialize(cfg);
  }

 private:
  fs::path root_;
};

}  // namespace

TEST_CASE("A backend keeps a thinking level through a round trip", "[backend]") {
  config::BackendConfig cfg;
  cfg.gateway = config::GatewayId::kOpenAI;
  cfg.host = "http://127.0.0.1:1";
  cfg.model = "test-model";
  cfg.thinking = ThinkingLevel::kHigh;

  const config::BackendConfig read =
      boost::json::value_to<config::BackendConfig>(boost::json::value_from(cfg));

  REQUIRE(read.thinking == ThinkingLevel::kHigh);
  REQUIRE(read.model == "test-model");
}

TEST_CASE("A backend keeps its type through a round trip", "[backend]") {
  for (const config::GatewayId type : {config::GatewayId::kOllama, config::GatewayId::kOpenAI,
                                         config::GatewayId::kCodeBuddy}) {
    config::BackendConfig cfg;
    cfg.gateway = type;
    cfg.host = "https://example.invalid/v1";
    cfg.model = "test-model";

    const config::BackendConfig read =
        boost::json::value_to<config::BackendConfig>(boost::json::value_from(cfg));

    REQUIRE(read.gateway == type);
    REQUIRE(std::string(config::GatewayName(type)) == config::GatewayName(read.gateway));
  }
}

TEST_CASE("A stored backend naming a gate this build has not got is refused", "[backend]") {
  const boost::json::value stored = {
      {"gateway", "some-gate"}, {"host", "https://example.invalid/v1"}, {"model", "test-model"}};

  REQUIRE_THROWS_AS(boost::json::value_to<config::BackendConfig>(stored), pu::Error);
}

TEST_CASE("A type is named the same way in configuration and in an answer", "[backend]") {
  REQUIRE(config::ParseGatewayId("codebuddy") == config::GatewayId::kCodeBuddy);
  REQUIRE(config::ParseGatewayId("openai") == config::GatewayId::kOpenAI);
  REQUIRE(config::ParseGatewayId("ollama") == config::GatewayId::kOllama);
  REQUIRE_FALSE(config::ParseGatewayId("gpt").has_value());

  REQUIRE(std::string(config::GatewayName(config::GatewayId::kCodeBuddy)) == "codebuddy");
}

TEST_CASE("A session spec carries only the agent and the thinking override", "[backend]") {
  SessionSpec chosen;
  chosen.agent_name = "chat";

  const boost::json::value written = chosen.Serialize();
  REQUIRE(written.as_object().count("agent_name") == 1);
  REQUIRE(written.as_object().count("thinking_override") == 0);

  chosen.thinking_override = ThinkingLevel::kLow;

  const auto restored = SessionSpec::Deserialize(chosen.Serialize());
  REQUIRE(restored.has_value());
  REQUIRE(restored->agent_name == "chat");
  REQUIRE(restored->thinking_override == ThinkingLevel::kLow);
}

TEST_CASE("Editing agents.json is enough to change the backend", "[backend]") {
  BackendSourceFixture fixture;

  {
    ScopedWorkingDir in_workspace(fixture.root());
    Runtime runtime;
    runtime.Initialize();
    REQUIRE(runtime.CurrentBackend().temperature == Catch::Approx(0.3f));
  }

  fixture.Write(0.9f);

  {
    ScopedWorkingDir in_workspace(fixture.root());
    Runtime runtime;
    runtime.Initialize();
    REQUIRE(runtime.CurrentBackend().temperature == Catch::Approx(0.9f));
  }
}

TEST_CASE("Switching the backend adopts an agent and keeps the tooling", "[backend]") {
  BackendSourceFixture fixture;
  ScopedWorkingDir in_workspace(fixture.root());
  Runtime runtime;
  runtime.Initialize();

  REQUIRE(runtime.CurrentBackend().temperature == Catch::Approx(0.3f));
  const auto names_before = runtime.GetAgentManager().GetAgentNames();

  config::BackendConfig replacement = runtime.CurrentBackend();
  replacement.temperature = 1.5f;
  runtime.SwitchBackend(replacement);

  REQUIRE(runtime.CurrentBackend().temperature == Catch::Approx(1.5f));
  REQUIRE(runtime.GetOrCreateDefaultSession() != nullptr);
  REQUIRE(runtime.GetOrCreateDefaultSession()->GetSpec().agent_name == "openai");

  const auto names_after = runtime.GetAgentManager().GetAgentNames();
  REQUIRE(names_after.size() == names_before.size() + 1);
  REQUIRE(runtime.GetAgentManager().GetAgentConfig("openai") != nullptr);
}

TEST_CASE("An agent named on the command line outranks the one a session remembers",
          "[backend]") {
  BackendSourceFixture fixture;

  {
    ScopedWorkingDir in_workspace(fixture.root());
    Runtime runtime;
    runtime.Initialize();
    const auto* coder = runtime.GetAgentManager().GetAgentConfig("coder");
    REQUIRE(coder != nullptr);
    runtime.SwitchAgent(*coder);
    runtime.Shutdown();
  }

  {
    ScopedWorkingDir in_workspace(fixture.root());
    Runtime runtime;
    runtime.SetDefaultAgent("chat");
    runtime.Initialize();

    REQUIRE(runtime.ActiveAgent().name == "chat");
    REQUIRE(runtime.GetOrCreateDefaultSession()->GetSpec().agent_name == "chat");
    REQUIRE(runtime.CurrentBackend().model == "test-model");
  }
}

TEST_CASE("A restart keeps talking to the agent the session names", "[backend]") {
  BackendSourceFixture fixture;

  {
    ScopedWorkingDir in_workspace(fixture.root());
    Runtime runtime;
    runtime.Initialize();
    REQUIRE(runtime.GetOrCreateDefaultSession() != nullptr);
    const auto* coder = runtime.GetAgentManager().GetAgentConfig("coder");
    REQUIRE(coder != nullptr);
    runtime.SwitchAgent(*coder);
    runtime.Shutdown();
  }

  {
    ScopedWorkingDir in_workspace(fixture.root());
    Runtime runtime;
    runtime.Initialize();
    REQUIRE(runtime.GetAgentManager().GetActiveAgent() == "coder");
    REQUIRE(runtime.CurrentBackend().model == "coder-model");
  }
}

TEST_CASE("A turn after an agent switch reaches an initialized registry", "[backend]") {
  BackendSourceFixture fixture;

  ScopedWorkingDir in_workspace(fixture.root());
  Runtime runtime;
  runtime.Initialize();
  REQUIRE(runtime.GetOrCreateDefaultSession() != nullptr);

  const auto* coder = runtime.GetAgentManager().GetAgentConfig("coder");
  REQUIRE(coder != nullptr);
  runtime.SwitchAgent(*coder);
  REQUIRE(runtime.GetAgentManager().GetActiveAgent() == "coder");

  bool is_command = false;
  const ExecutionResult result = runtime.ProcessInput("hello", is_command);

  REQUIRE(is_command == false);
  REQUIRE(result.has_error);

  runtime.Shutdown();
}

TEST_CASE("A session this build cannot read is kept, not overwritten", "[backend]") {
  BackendSourceFixture fixture;
  const fs::path session_path = fixture.root() / ".pu" / "session.json";
  const fs::path backup_path = fixture.root() / ".pu" / "session.json.bak";

  const std::string unreadable =
      R"({"schema_version": 6, "conversation": {"history": {}}, "session_spec": {"agent_name": "chat"}})";
  {
    std::ofstream out(session_path, std::ios::trunc);
    out << unreadable;
  }

  {
    ScopedWorkingDir in_workspace(fixture.root());
    Runtime runtime;
    runtime.Initialize();
    runtime.Shutdown();
  }

  std::ifstream backup(backup_path, std::ios::binary);
  REQUIRE(backup.is_open());
  const std::string kept((std::istreambuf_iterator<char>(backup)),
                         std::istreambuf_iterator<char>());
  REQUIRE(kept == unreadable);

  // The path no longer holds what could not be read: either the fresh conversation has
  // not been saved yet, or what stands there is that fresh conversation.
  const auto holds_unreadable = [&] {
    std::ifstream in(session_path, std::ios::binary);
    if (!in.is_open()) return false;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text == unreadable;
  }();
  REQUIRE_FALSE(holds_unreadable);
}
