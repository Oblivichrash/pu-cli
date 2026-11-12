// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "pu/config/agents.hpp"
#include "pu/runtime/runtime.hpp"
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
  model::Backend cfg;
  cfg.gateway = model::GatewayId::kOpenAI;
  cfg.host = "http://127.0.0.1:1";
  cfg.model = "test-model";
  cfg.thinking = ThinkingLevel::kHigh;

  const model::Backend read =
      boost::json::value_to<model::Backend>(boost::json::value_from(cfg));

  REQUIRE(read.thinking == ThinkingLevel::kHigh);
  REQUIRE(read.model == "test-model");
}

TEST_CASE("A backend keeps its type through a round trip", "[backend]") {
  for (const model::GatewayId type : {model::GatewayId::kOllama, model::GatewayId::kOpenAI,
                                         model::GatewayId::kCodeBuddy}) {
    model::Backend cfg;
    cfg.gateway = type;
    cfg.host = "https://example.invalid/v1";
    cfg.model = "test-model";

    const model::Backend read =
        boost::json::value_to<model::Backend>(boost::json::value_from(cfg));

    REQUIRE(read.gateway == type);
  }
}

TEST_CASE("A stored backend naming a gateway this build has not got is refused", "[backend]") {
  const boost::json::value stored = {
      {"gateway", "some-gateway"}, {"host", "https://example.invalid/v1"}, {"model", "test-model"}};

  REQUIRE_THROWS_AS(boost::json::value_to<model::Backend>(stored), pu::GenericError);
}

TEST_CASE("A stored backend naming no gateway is refused rather than defaulted", "[backend]") {
  // The field was named `type` before the gateway table; a file still carrying it names no
  // gateway, and reading it as ollama would send its turns to a host nobody chose.
  for (const boost::json::value stored :
       {boost::json::value{{"host", "https://example.invalid/v1"}, {"model", "test-model"}},
        boost::json::value{{"type", "openai"}, {"host", "https://example.invalid/v1"},
                           {"model", "test-model"}},
        boost::json::value{{"gateway", ""}, {"model", "test-model"}}}) {
    REQUIRE_THROWS_AS(boost::json::value_to<model::Backend>(stored), pu::GenericError);
  }
}

TEST_CASE("A type is named the same way in configuration and in an answer", "[backend]") {
  REQUIRE(model::ParseGatewayId("codebuddy") == model::GatewayId::kCodeBuddy);
  REQUIRE(model::ParseGatewayId("openai") == model::GatewayId::kOpenAI);
  REQUIRE(model::ParseGatewayId("ollama") == model::GatewayId::kOllama);
  REQUIRE_FALSE(model::ParseGatewayId("gpt").has_value());

  REQUIRE(std::string(model::GatewayName(model::GatewayId::kCodeBuddy)) == "codebuddy");
}

TEST_CASE("A session spec carries only the agent and the thinking override", "[backend]") {
  SessionSpec chosen;
  chosen.agent_name = "chat";

  const boost::json::value written = chosen.Serialize();
  REQUIRE(written.as_object().count("agent_name") == 1);
  REQUIRE(written.as_object().count("thinking_override") == 0);
  REQUIRE(written.as_object().count("tool_budget_override") == 0);

  chosen.overrides["thinking"] = boost::json::value("low");
  chosen.overrides["tool_budget"] = 7;

  const auto restored = SessionSpec::Deserialize(chosen.Serialize());
  REQUIRE(restored.has_value());
  REQUIRE(restored->agent_name == "chat");
  REQUIRE(restored->overrides.at("thinking") == "low");
  REQUIRE(restored->overrides.at("tool_budget") == 7);
  REQUIRE(restored->ThinkingOverride() == ThinkingLevel::kLow);
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
  const auto names_before = runtime.Agents().AgentNames();

  model::Backend replacement = runtime.CurrentBackend();
  replacement.temperature = 1.5f;
  runtime.SwitchBackend(replacement);

  REQUIRE(runtime.CurrentBackend().temperature == Catch::Approx(1.5f));
  REQUIRE(runtime.DefaultSession() != nullptr);
  REQUIRE(runtime.DefaultSession()->Spec().agent_name == "openai");

  const auto names_after = runtime.Agents().AgentNames();
  REQUIRE(names_after.size() == names_before.size() + 1);
  REQUIRE(runtime.Agents().AgentConfig("openai") != nullptr);
}

TEST_CASE("An agent named on the command line outranks the one a session remembers",
          "[backend]") {
  BackendSourceFixture fixture;

  {
    ScopedWorkingDir in_workspace(fixture.root());
    Runtime runtime;
    runtime.Initialize();
    const auto* coder = runtime.Agents().AgentConfig("coder");
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
    REQUIRE(runtime.DefaultSession()->Spec().agent_name == "chat");
    REQUIRE(runtime.CurrentBackend().model == "test-model");
  }
}

TEST_CASE("A restart keeps talking to the agent the session names", "[backend]") {
  BackendSourceFixture fixture;

  {
    ScopedWorkingDir in_workspace(fixture.root());
    Runtime runtime;
    runtime.Initialize();
    REQUIRE(runtime.DefaultSession() != nullptr);
    const auto* coder = runtime.Agents().AgentConfig("coder");
    REQUIRE(coder != nullptr);
    runtime.SwitchAgent(*coder);
    runtime.Shutdown();
  }

  {
    ScopedWorkingDir in_workspace(fixture.root());
    Runtime runtime;
    runtime.Initialize();
    REQUIRE(runtime.Agents().ActiveAgentName() == "coder");
    REQUIRE(runtime.CurrentBackend().model == "coder-model");
  }
}

TEST_CASE("A turn after an agent switch reaches an initialized registry", "[backend]") {
  BackendSourceFixture fixture;

  ScopedWorkingDir in_workspace(fixture.root());
  Runtime runtime;
  runtime.Initialize();
  REQUIRE(runtime.DefaultSession() != nullptr);

  const auto* coder = runtime.Agents().AgentConfig("coder");
  REQUIRE(coder != nullptr);
  runtime.SwitchAgent(*coder);
  REQUIRE(runtime.Agents().ActiveAgentName() == "coder");

  const ExecutionResult result = runtime.ProcessInput("hello");

  REQUIRE(result.was_command == false);
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

TEST_CASE("A gateway name is one product, and the table answers for all of them", "[backend]") {
  for (const char* name : {"ollama", "ollama-cloud", "openai", "deepseek", "nim", "codebuddy"}) {
    const std::optional<model::GatewayId> id = model::ParseGatewayId(name);
    REQUIRE(id.has_value());
    REQUIRE(std::string(model::GatewayName(*id)) == name);
    REQUIRE_FALSE(model::DefaultHostFor(*id).empty());
  }
}

TEST_CASE("Naming a gateway reaches it without an agents.json entry", "[backend]") {
  const model::Backend nim = model::BackendForGateway("nim");
  REQUIRE(nim.gateway == model::GatewayId::kNim);
  REQUIRE(nim.host == "https://integrate.api.nvidia.com/v1");

  // Two products on one protocol keep their own hosts, which is why the gateway names the
  // product and not the wire format.
  REQUIRE(model::BackendForGateway("deepseek").host == "https://api.deepseek.com/v1");
  REQUIRE(model::BackendForGateway("openai").host == "https://api.openai.com/v1");
}

TEST_CASE("Naming a gateway this build has not got says which it has", "[backend]") {
  try {
    (void)model::BackendForGateway("frobnicate");
    FAIL("a gateway this build has not got was accepted");
  } catch (const pu::GenericError& e) {
    const std::string message = e.what();
    REQUIRE(message.find("Unknown gateway") != std::string::npos);
    // The candidates, since a name typed from memory is the case this guards.
    REQUIRE(message.find("nim") != std::string::npos);
    REQUIRE(message.find("codebuddy") != std::string::npos);
  }
}
