// SPDX-License-Identifier: GPL-3.0-only

//
// The one list of configured agents, and the four surfaces that print it.
//
// `pu agents`, the chat's `/agents`, `GET /api/agents` and the Web selector answer the same
// question. They differ in how they draw the answer; these cases hold the answer still, so a
// change to one surface's wording cannot quietly change what the others say.
//

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <string>
#include <vector>

#include "pu/config/agent_summary.hpp"
#include "pu/core/json.hpp"

namespace {

pu::config::AgentSummary AgentOf(const std::string& name, const std::string& description,
                                 const std::string& gateway, const std::string& model,
                                 bool active) {
  pu::config::AgentSummary agent;
  agent.name = name;
  agent.description = description;
  agent.gateway = gateway;
  agent.model = model;
  agent.active = active;
  return agent;
}

}  // namespace

TEST_CASE("The JSON shape is the one the API answers with", "[agents]") {
  const std::vector<pu::config::AgentSummary> agents = {
      AgentOf("fast", "the cheap one", "codebuddy", "flash", true),
      AgentOf("strong", "", "openai", "pro", false),
  };

  const boost::json::value json = pu::config::AgentsAsJson(agents);
  REQUIRE(json.is_array());
  REQUIRE(json.as_array().size() == 2);

  const boost::json::value& first = json.as_array().at(0);
  REQUIRE(pu::json::ValueOrDefault<std::string>(first, "name", "") == "fast");
  REQUIRE(pu::json::ValueOrDefault<std::string>(first, "description", "") == "the cheap one");
  REQUIRE(pu::json::ValueOrDefault<std::string>(first, "gateway", "") == "codebuddy");
  REQUIRE(pu::json::ValueOrDefault<std::string>(first, "model", "") == "flash");
  REQUIRE(pu::json::ValueOrDefault<bool>(first, "active", false));

  // An agent with no description still carries the field: a reader of the shape should not
  // have to tell "absent" from "empty".
  const boost::json::value& second = json.as_array().at(1);
  REQUIRE(second.as_object().contains("description"));
  REQUIRE_FALSE(pu::json::ValueOrDefault<bool>(second, "active", true));
}

TEST_CASE("The table marks the active agent and says what each one is", "[agents]") {
  const std::vector<pu::config::AgentSummary> agents = {
      AgentOf("fast", "the cheap one", "codebuddy", "flash", true),
      AgentOf("strong-name", "", "openai", "pro", false),
  };

  const std::string table = pu::config::FormatAgentTable(agents);
  REQUIRE(table.find("2 agent(s) configured") != std::string::npos);
  REQUIRE(table.find("fast (active)") != std::string::npos);
  // Only the active one is marked, or the marker says nothing.
  REQUIRE(table.find("strong-name (active)") == std::string::npos);
  REQUIRE(table.find("the cheap one") != std::string::npos);
  // The gateway and the model, which is what tells two agents apart after the name.
  REQUIRE(table.find("codebuddy / flash") != std::string::npos);
  REQUIRE(table.find("openai / pro") != std::string::npos);
}

TEST_CASE("An empty configuration says so instead of printing nothing", "[agents]") {
  // A blank answer reads as a broken command; the words say what is missing.
  REQUIRE(pu::config::FormatAgentTable({}) == "No agents are configured.\n");
  REQUIRE(pu::config::AgentsAsJson({}).as_array().empty());
}
