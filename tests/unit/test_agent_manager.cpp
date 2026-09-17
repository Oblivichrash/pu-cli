// SPDX-License-Identifier: GPL-3.0-only
#include "pu/agent_manager.hpp"
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <vector>
#include <string>

using namespace pu;

TEST_CASE("AgentManager GetAgentNames returns names from configs", "[agent]") {
  AgentManager manager;

  config::AgentEntry entry1;
  entry1.name = "agent1";
  entry1.description = "First agent";

  config::AgentEntry entry2;
  entry2.name = "agent2";
  entry2.description = "Second agent";

  manager.LoadAgentConfigs({entry1, entry2});

  auto names = manager.GetAgentNames();
  REQUIRE(names.size() == 2);
  REQUIRE(names[0] == "agent1");
  REQUIRE(names[1] == "agent2");
}

TEST_CASE("AgentManager SetActiveAgent and GetActiveAgent work", "[agent]") {
  AgentManager manager;

  manager.SetActiveAgent("test_agent");
  REQUIRE(manager.GetActiveAgent() == "test_agent");
}

TEST_CASE("AgentManager GetAgentConfig finds entry by name", "[agent]") {
  AgentManager manager;

  config::AgentEntry entry;
  entry.name = "test";
  entry.description = "Test agent";

  manager.LoadAgentConfigs({entry});

  const auto* found = manager.GetAgentConfig("test");
  REQUIRE(found != nullptr);
  REQUIRE(found->name == "test");
  REQUIRE(found->description == "Test agent");

  const auto* not_found = manager.GetAgentConfig("nonexistent");
  REQUIRE(not_found == nullptr);
}

TEST_CASE("AgentManager LoadAgentConfigs replaces previous configs", "[agent]") {
  AgentManager manager;

  config::AgentEntry old_entry;
  old_entry.name = "old";
  old_entry.description = "Old agent";

  config::AgentEntry new_entry;
  new_entry.name = "new";
  new_entry.description = "New agent";

  manager.LoadAgentConfigs({old_entry});
  REQUIRE(manager.GetAgentNames().size() == 1);
  REQUIRE(manager.GetAgentNames()[0] == "old");

  manager.LoadAgentConfigs({new_entry});

  auto names = manager.GetAgentNames();
  REQUIRE(names.size() == 1);
  REQUIRE(names[0] == "new");
  REQUIRE(manager.GetAgentConfig("old") == nullptr);
  REQUIRE(manager.GetAgentConfig("new") != nullptr);
}
