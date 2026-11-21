// SPDX-License-Identifier: GPL-3.0-only

//
// The one agent, and the surfaces that print it.
//
// `pu agents`, the chat's `/agents` and `GET /api/agents` answer the same question. They
// differ in how they draw the answer; these cases hold the answer still, so a change to one
// surface's wording cannot quietly change what the others say.
//

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <string>

#include "pu/app/agent_summary.hpp"
#include "pu/core/json.hpp"

namespace {

pu::app::AgentSummary AgentOf(const std::string& name, const std::string& description,
                              const std::string& gateway, const std::string& model) {
  pu::app::AgentSummary agent;
  agent.name = name;
  agent.description = description;
  agent.gateway = gateway;
  agent.model = model;
  return agent;
}

}  // namespace

TEST_CASE("The JSON shape is the one the API answers with", "[agents]") {
  const boost::json::value json =
      pu::app::AgentAsJson(AgentOf("fast", "the cheap one", "tencent", "flash"));
  REQUIRE(json.is_object());

  REQUIRE(pu::json::ValueOrDefault<std::string>(json, "name", "") == "fast");
  REQUIRE(pu::json::ValueOrDefault<std::string>(json, "description", "") == "the cheap one");
  REQUIRE(pu::json::ValueOrDefault<std::string>(json, "gateway", "") == "tencent");
  REQUIRE(pu::json::ValueOrDefault<std::string>(json, "model", "") == "flash");

  // An agent with no description still carries the field: a reader of the shape should not
  // have to tell "absent" from "empty".
  const boost::json::value bare = pu::app::AgentAsJson(AgentOf("fast", "", "tencent", "flash"));
  REQUIRE(bare.as_object().contains("description"));
}

TEST_CASE("The lines say what the agent is and which product answers it", "[agents]") {
  const std::string lines =
      pu::app::FormatAgent(AgentOf("fast", "the cheap one", "tencent", "flash"));
  REQUIRE(lines.rfind("fast\n", 0) == 0);
  REQUIRE(lines.find("the cheap one") != std::string::npos);
  REQUIRE(lines.find("tencent / flash") != std::string::npos);
}

TEST_CASE("An empty configuration says so instead of printing nothing", "[agents]") {
  // A blank answer reads as a broken command; the words say what is missing.
  REQUIRE(pu::app::FormatAgent({}) == "No agent is configured.\n");
  REQUIRE(pu::app::AgentAsJson({}).is_null());
}
