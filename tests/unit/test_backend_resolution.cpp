// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <filesystem>
#include <fstream>
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
                      {"type", "openai"},
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
                      {"type", "openai"},
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
  cfg.type = config::BackendType::kOpenAI;
  cfg.host = "http://127.0.0.1:1";
  cfg.model = "test-model";
  cfg.thinking = ThinkingLevel::kHigh;

  const config::BackendConfig read =
      boost::json::value_to<config::BackendConfig>(boost::json::value_from(cfg));

  REQUIRE(read.thinking == ThinkingLevel::kHigh);
  REQUIRE(read.model == "test-model");
}

TEST_CASE("A backend keeps its type through a round trip", "[backend]") {
  for (const config::BackendType type : {config::BackendType::kOllama, config::BackendType::kOpenAI,
                                         config::BackendType::kCodeBuddy}) {
    config::BackendConfig cfg;
    cfg.type = type;
    cfg.host = "https://example.invalid/v1";
    cfg.model = "test-model";

    const config::BackendConfig read =
        boost::json::value_to<config::BackendConfig>(boost::json::value_from(cfg));

    REQUIRE(read.type == type);
    REQUIRE(std::string(config::BackendTypeName(type)) == config::BackendTypeName(read.type));
  }
}

TEST_CASE("A type is named the same way in configuration and in an answer", "[backend]") {
  REQUIRE(config::ParseBackendType("codebuddy") == config::BackendType::kCodeBuddy);
  REQUIRE(config::ParseBackendType("openai") == config::BackendType::kOpenAI);
  REQUIRE(config::ParseBackendType("ollama") == config::BackendType::kOllama);
  REQUIRE_FALSE(config::ParseBackendType("gpt").has_value());

  REQUIRE(std::string(config::BackendTypeName(config::BackendType::kCodeBuddy)) == "codebuddy");
}

TEST_CASE("A session spec carries a backend only when one was overridden", "[backend]") {
  SessionSpec chosen;
  chosen.agent_name = "chat";

  const boost::json::value written = chosen.Serialize();
  REQUIRE(written.as_object().count("agent_name") == 1);
  REQUIRE(written.as_object().count("backend_override") == 0);

  config::BackendConfig override_cfg;
  override_cfg.model = "overridden";
  override_cfg.thinking = ThinkingLevel::kLow;
  chosen.backend_override = override_cfg;

  const auto restored = SessionSpec::Deserialize(chosen.Serialize());
  REQUIRE(restored.has_value());
  REQUIRE(restored->agent_name == "chat");
  REQUIRE(restored->backend_override.has_value());
  REQUIRE(restored->backend_override->model == "overridden");
  REQUIRE(restored->backend_override->thinking == ThinkingLevel::kLow);
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

TEST_CASE("A session override wins over the configured backend", "[backend]") {
  BackendSourceFixture fixture;
  ScopedWorkingDir in_workspace(fixture.root());
  Runtime runtime;
  runtime.Initialize();

  auto session = runtime.GetOrCreateDefaultSession();
  REQUIRE(session != nullptr);
  REQUIRE(runtime.CurrentBackend().temperature == Catch::Approx(0.3f));

  config::BackendConfig override_cfg = runtime.CurrentBackend();
  override_cfg.temperature = 1.5f;
  session->SetBackendOverride(override_cfg);

  REQUIRE(runtime.CurrentBackend().temperature == Catch::Approx(1.5f));
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

TEST_CASE("A turn after an agent switch still has a toolbox to read", "[backend]") {
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
  REQUIRE(result.error_message.find("Tool registry is not initialized") == std::string::npos);

  runtime.Shutdown();
}
