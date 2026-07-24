// SPDX-License-Identifier: GPL-3.0-only
#include "pu/agent_core.hpp"
#include "pu/conversation.hpp"
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <vector>

using namespace pu;
using namespace pu::agent;

namespace {

class MockAgent : public BaseAgent {
 public:
  explicit MockAgent(std::string name) : name_(std::move(name)) {}

  std::string Name() const override { return name_; }
  std::string Description() const override { return "Mock"; }
  std::string Handle(const std::string&, AgentContext&) override {
    return "mock response";
  }
  void ResetSession() override { state_.clear(); }

  std::vector<ChatMessage> SaveState() const override {
    return state_;
  }
  void LoadState(const std::vector<ChatMessage>& messages) override {
    state_ = messages;
  }

  void AddFakeMessage(ChatMessage msg) {
    state_.push_back(std::move(msg));
  }

 private:
  std::string name_;
  std::vector<ChatMessage> state_;
};

}  // namespace

TEST_CASE("AgentManager SnapshotAgents collects all states", "[agent]") {
  AgentManager manager;

  auto agent1 = std::make_unique<MockAgent>("mock1");
  agent1->AddFakeMessage({1, "", "user", "hello"});
  agent1->AddFakeMessage({2, "", "mock1", "hi"});

  auto agent2 = std::make_unique<MockAgent>("mock2");
  agent2->AddFakeMessage({3, "", "user", "test"});

  manager.RegisterAgent(std::move(agent1));
  manager.RegisterAgent(std::move(agent2));

  auto snapshot = manager.SnapshotAgents();
  REQUIRE(snapshot.size() == 2);
  REQUIRE(snapshot["mock1"].size() == 2);
  REQUIRE(snapshot["mock2"].size() == 1);
  REQUIRE(snapshot["mock1"][0].content == "hello");
}

TEST_CASE("AgentManager RestoreAgents loads states", "[agent]") {
  AgentManager manager;

  auto agent = std::make_unique<MockAgent>("mock1");
  manager.RegisterAgent(std::move(agent));

  std::unordered_map<std::string, std::vector<ChatMessage>> states;
  states["mock1"] = {
    {1, "", "user", "restored message"}
  };

  manager.RestoreAgents(states);

  auto snapshot = manager.SnapshotAgents();
  REQUIRE(snapshot["mock1"].size() == 1);
  REQUIRE(snapshot["mock1"][0].content == "restored message");
}

TEST_CASE("AgentManager RestoreAgents ignores unknown experts", "[agent]") {
  AgentManager manager;

  std::unordered_map<std::string, std::vector<ChatMessage>> states;
  states["nonexistent"] = {{1, "", "user", "nobody"}};

  REQUIRE_NOTHROW(manager.RestoreAgents(states));
}

TEST_CASE("AgentManager ClearSessions resets all states", "[agent]") {
  AgentManager manager;

  auto agent = std::make_unique<MockAgent>("mock1");
  agent->AddFakeMessage({1, "", "user", "data"});
  manager.RegisterAgent(std::move(agent));

  manager.ClearSessions();

  auto snapshot = manager.SnapshotAgents();
  REQUIRE(snapshot["mock1"].empty());
}
