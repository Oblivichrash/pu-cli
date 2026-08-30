// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "pu/agent_manager.hpp"
#include "pu/command_router.hpp"
#include "pu/runtime.hpp"
#include "pu/session/session.hpp"

#ifdef _WIN32
// Windows does not have setenv; use _putenv_s instead.
static inline void setenv(const char* name, const char* value, int /*overwrite*/) {
  _putenv_s(name, value);
}
#endif

using namespace pu;

namespace {

std::string MakeTempHome() {
  static int counter = 0;
  auto dir = std::filesystem::temp_directory_path() /
             ("pu_command_router_test_" + std::to_string(++counter));
  std::filesystem::create_directories(dir);
  return dir.string();
}

struct RouterFixture {
  AgentManager manager;
  Runtime runtime;
  Session session;
  CommandRouter router;

  RouterFixture() : router(manager, runtime) {
    setenv("PU_HOME", MakeTempHome().c_str(), 1);

    config::AgentEntry chat;
    chat.name = "chat";
    chat.description = "Default agent";
    config::AgentEntry coder;
    coder.name = "coder";
    coder.description = "Coding agent";
    manager.LoadAgentConfigs({chat, coder});
    manager.SetActiveAgent("chat");
    session.SwitchAgent("chat");
  }

  bool Route(const std::string& input, std::string& output) {
    return router.Route(input, session, output);
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

TEST_CASE("CommandRouter handles /exit and /quit outside the registry",
          "[router]") {
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

TEST_CASE("CommandRouter validates arguments with RequireMinArgs", "[router]") {
  RouterFixture f;
  std::string output;

  REQUIRE(f.Route("/note", output));
  REQUIRE(output == "Usage: /note add <text> | show");
}

TEST_CASE("CommandRouter /note add and show roundtrip per agent", "[router]") {
  RouterFixture f;
  std::string output;

  REQUIRE(f.Route("/note add remember this", output));
  REQUIRE(output == "Note added.");

  REQUIRE(f.Route("/note show", output));
  REQUIRE(output.find("remember this") != std::string::npos);
}