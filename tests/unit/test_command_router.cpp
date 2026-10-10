// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "pu/agent_manager.hpp"
#include "pu/command_router.hpp"
#include "pu/llm/codebuddy.hpp"
#include "pu/runtime.hpp"
#include "pu/session/session.hpp"
#include "tests/mocks/test_helpers.hpp"

using namespace pu;

namespace {

struct RouterFixture {
  pu::tests::ScopedTempDir root{"pu_command_router_test_"};
  std::unique_ptr<pu::tests::ScopedEnvVar> home;
  std::unique_ptr<pu::tests::ScopedWorkingDir> cwd;

  Runtime runtime;
  std::unique_ptr<CommandRouter> router;

  RouterFixture() {
    std::filesystem::create_directories(root.Path() / ".pu");
    WriteAgentsFile();

    home = std::make_unique<pu::tests::ScopedEnvVar>("PU_HOME", root.Path().string());
    cwd = std::make_unique<pu::tests::ScopedWorkingDir>(root.Path());

    runtime.Initialize();
    router = std::make_unique<CommandRouter>(runtime);
  }

  bool Route(const std::string& input, std::string& output) {
    return router->Route(input, *runtime.GetOrCreateDefaultSession(), output);
  }

 private:
  void WriteAgentsFile() const {
    boost::json::value cfg = {
        {"default_agent", "chat"},
        {"agents",
         boost::json::array{
             boost::json::value{{"name", "chat"},
                                {"description", "Default agent"},
                                {"backend",
                                 {{"type", "ollama"},
                                  {"host", "http://127.0.0.1:11434"},
                                  {"model", "chat-model"}}}},
             boost::json::value{{"name", "coder"},
                                {"description", "Coding agent"},
                                {"backend",
                                 {{"type", "ollama"},
                                  {"host", "http://127.0.0.1:11434"},
                                  {"model", "coder-model"}}}},
         }},
    };
    std::ofstream out(root.Path() / ".pu" / "agents.json", std::ios::trunc);
    out << boost::json::serialize(cfg);
  }
};

}  // namespace

TEST_CASE("CommandRouter dispatches registered commands through the registry", "[router]") {
  RouterFixture f;
  std::string output;

  REQUIRE(f.Route("/help", output));
  REQUIRE(output.find("/help") != std::string::npos);
  REQUIRE(output.find("/backend") != std::string::npos);
  REQUIRE(output.find("/exit, /quit") != std::string::npos);

  REQUIRE(f.Route("/clear", output));
  REQUIRE(output == "Conversation history cleared.");

  REQUIRE(f.Route("/agents", output));
  REQUIRE(output.find("chat") != std::string::npos);
  REQUIRE(output.find("coder") != std::string::npos);
}

TEST_CASE("CommandRouter handles /exit and /quit outside the registry", "[router]") {
  RouterFixture f;
  std::string output;

  REQUIRE(f.Route("/exit", output));
  REQUIRE(output.empty());
  REQUIRE(f.Route("/quit", output));
  REQUIRE(output.empty());
}

TEST_CASE("CommandRouter rejects unknown and non-command input", "[router]") {
  RouterFixture f;
  std::string output;

  REQUIRE_FALSE(f.Route("/definitely-not-a-command", output));
  REQUIRE_FALSE(f.Route("just a message", output));
  REQUIRE_FALSE(f.Route("", output));
  REQUIRE_FALSE(f.Route("   ", output));
}

TEST_CASE("CommandRouter rejects a command it does not have", "[router]") {
  RouterFixture f;
  std::string output;

  REQUIRE_FALSE(f.Route("/unknown", output));
  REQUIRE_FALSE(f.Route("/unknown add remember this", output));
  REQUIRE_FALSE(f.Route("/unknown show", output));
}

TEST_CASE("CommandRouter gives /backend the host its type implies", "[router]") {
  RouterFixture f;
  std::string output;

  REQUIRE(f.Route("/backend codebuddy deepseek-v4-flash", output));
  REQUIRE(output.find(llm::kCodeBuddyHost) != std::string::npos);

  REQUIRE(f.Route("/backend ollama llama3", output));
  REQUIRE(output.find("http://localhost:11434") != std::string::npos);

  REQUIRE(f.Route("/backend gpt whatever", output));
  REQUIRE(output.find("Unknown type") != std::string::npos);
}

TEST_CASE("CommandRouter reports one current state for /backend and /agents", "[router]") {
  RouterFixture f;
  std::string output;

  REQUIRE(f.Route("/agents", output));
  REQUIRE(output.find("chat (active)") != std::string::npos);

  REQUIRE(f.Route("/backend codebuddy deepseek-v4-flash", output));
  REQUIRE(output.find("codebuddy") != std::string::npos);

  REQUIRE(f.Route("/agents", output));
  REQUIRE(output.find("codebuddy (active)") != std::string::npos);

  REQUIRE(f.Route("/backend", output));
  REQUIRE(output.find("codebuddy") != std::string::npos);

  REQUIRE(f.Route("/backend coder", output));
  REQUIRE(output.find("Switched to agent: coder") != std::string::npos);

  REQUIRE(f.Route("/agents", output));
  REQUIRE(output.find("coder (active)") != std::string::npos);
  REQUIRE(output.find("chat (active)") == std::string::npos);
}

TEST_CASE("Switching to a backend leaves a same-named configured agent alone", "[router]") {
  RouterFixture f;
  std::string output;

  REQUIRE(f.Route("/backend ollama llama3", output));
  REQUIRE(output.find("Switched backend to") != std::string::npos);

  const auto* chat = f.runtime.GetAgentManager().GetAgentConfig("chat");
  REQUIRE(chat != nullptr);
  REQUIRE(chat->backend.model == "chat-model");

  REQUIRE(f.Route("/backend chat", output));
  REQUIRE(output.find("Switched to agent: chat") != std::string::npos);
  REQUIRE(output.find("chat-model") != std::string::npos);
}
