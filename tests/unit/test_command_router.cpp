// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "pu/runtime/command_router.hpp"
#include "pu/runtime/runtime.hpp"
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

  CommandReply Route(const std::string& input) {
    return router->Route(input, *runtime.DefaultSession());
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
                                 {{"gateway", "ollama"},
                                  {"host", "http://127.0.0.1:11434"},
                                  {"model", "chat-model"}}}},
             boost::json::value{{"name", "coder"},
                                {"description", "Coding agent"},
                                {"backend",
                                 {{"gateway", "ollama"},
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
  CommandReply reply;
  std::string output;

  reply = f.Route("/help");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("/help") != std::string::npos);
  REQUIRE(output.find("/backend") != std::string::npos);
  REQUIRE(output.find("/exit, /quit") != std::string::npos);

  reply = f.Route("/clear");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output == "Conversation history cleared.");

  reply = f.Route("/agents");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("chat") != std::string::npos);
  REQUIRE(output.find("coder") != std::string::npos);
}

TEST_CASE("CommandRouter routes through the instance that was built last", "[router]") {
  // The table captures each instance's own `this`, so a second router has to route
  // through itself. A table shared across instances calls through the first one.
  {
    RouterFixture first;
    CommandReply reply;
    std::string output;
    reply = first.Route("/help");
    REQUIRE(reply.ok);
    output = reply.text;
    REQUIRE(output.find("/help") != std::string::npos);
  }

  RouterFixture second;
  CommandReply reply;
  std::string output;
  reply = second.Route("/help");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("/help") != std::string::npos);
  reply = second.Route("/agents");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("chat") != std::string::npos);
}

TEST_CASE("CommandRouter handles /exit and /quit outside the registry", "[router]") {
  RouterFixture f;

  for (const char* leaving : {"/exit", "/quit"}) {
    const CommandReply reply = f.Route(leaving);
    REQUIRE(reply.routed);
    REQUIRE(reply.ok);
    // Nothing to say, but not a refusal: leaving is what was asked for.
    REQUIRE(reply.text.empty());
  }
}

TEST_CASE("CommandRouter distinguishes a failed command from a line that is not one",
          "[router]") {
  RouterFixture f;

  // Routed, and answered with the reason it could not run.
  for (const char* unknown : {"/definitely-not-a-command", "/unknown",
                              "/unknown add remember this", "/unknown show"}) {
    const CommandReply reply = f.Route(unknown);
    REQUIRE(reply.routed);
    REQUIRE_FALSE(reply.ok);
    REQUIRE(reply.text.find("Unknown command") != std::string::npos);
  }

  // Not routed at all: this is a message for the provider, not a command that failed.
  for (const char* message : {"just a message", "", "   "}) {
    const CommandReply reply = f.Route(message);
    REQUIRE_FALSE(reply.routed);
    REQUIRE(reply.text.empty());
  }
}

TEST_CASE("CommandRouter gives /backend the host its type implies", "[router]") {
  RouterFixture f;
  CommandReply reply;
  std::string output;

  reply = f.Route("/backend codebuddy deepseek-v4-flash");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find(config::DefaultHostFor(config::GatewayId::kCodeBuddy)) != std::string::npos);

  reply = f.Route("/backend ollama llama3");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("http://localhost:11434") != std::string::npos);

  // Was a command, and said so as its answer: a bad type is the command's failure, not a
  // refusal to route. `ok` names only the second thing, so this reads false.
  reply = f.Route("/backend gpt whatever");
  REQUIRE(reply.routed);
  REQUIRE_FALSE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("Unknown type") != std::string::npos);
}

TEST_CASE("CommandRouter will not take a key at the prompt", "[router]") {
  RouterFixture f;
  CommandReply reply;
  std::string output;

  // A key typed here lands in the scrollback, in the terminal's own history, and in
  // whatever captures a screen, which is the opposite of what docs/security.md promises.
  // Refusing the argument must not half-apply it: the active agent is where it started.
  // Typed with too many words, the command is refused, which is what `ok` reports.
  reply = f.Route("/backend ollama llama3 http://localhost:11434 sk-secret");
  REQUIRE(reply.routed);
  REQUIRE_FALSE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("agents.json") != std::string::npos);
  REQUIRE(output.find("sk-secret") == std::string::npos);

  // Refusing the argument must not half-apply it: the active agent is where it started.
  reply = f.Route("/backend");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("chat-model") != std::string::npos);

  // Three arguments are still a host, and still accepted.
  reply = f.Route("/backend ollama llama3 http://localhost:11500");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("Switched backend to") != std::string::npos);
  REQUIRE(output.find("http://localhost:11500") != std::string::npos);
}

TEST_CASE("CommandRouter reports one current state for /backend and /agents", "[router]") {
  RouterFixture f;
  CommandReply reply;
  std::string output;

  reply = f.Route("/agents");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("chat (active)") != std::string::npos);

  reply = f.Route("/backend codebuddy deepseek-v4-flash");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("codebuddy") != std::string::npos);

  reply = f.Route("/agents");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("codebuddy (active)") != std::string::npos);

  reply = f.Route("/backend");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("codebuddy") != std::string::npos);

  reply = f.Route("/backend coder");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("Switched to agent: coder") != std::string::npos);

  reply = f.Route("/agents");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("coder (active)") != std::string::npos);
  REQUIRE(output.find("chat (active)") == std::string::npos);
}

TEST_CASE("CommandRouter names an adopted agent after switching to it", "[router]") {
  RouterFixture f;
  CommandReply reply;
  std::string output;

  // Adopting a backend puts a transient entry in the manager; switching to that entry
  // by name walks the same path a configured agent does.
  reply = f.Route("/backend ollama llama3");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("Switched backend to") != std::string::npos);

  reply = f.Route("/backend ollama");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("Switched to agent: ollama") != std::string::npos);
  REQUIRE(output.find("llama3") != std::string::npos);

  reply = f.Route("/agents");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("ollama (active)") != std::string::npos);
}

TEST_CASE("Switching to a backend leaves a same-named configured agent alone", "[router]") {
  RouterFixture f;
  CommandReply reply;
  std::string output;

  reply = f.Route("/backend ollama llama3");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("Switched backend to") != std::string::npos);

  const auto* chat = f.runtime.Agents().AgentConfig("chat");
  REQUIRE(chat != nullptr);
  REQUIRE(chat->backend.model == "chat-model");

  reply = f.Route("/backend chat");
  REQUIRE(reply.ok);
  output = reply.text;
  REQUIRE(output.find("Switched to agent: chat") != std::string::npos);
  REQUIRE(output.find("chat-model") != std::string::npos);
}
