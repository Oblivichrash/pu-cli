// SPDX-License-Identifier: GPL-3.0-only

//
// The agent a workspace configures, as a reader needs it.
//
// Three surfaces ask this question: `pu agents`, the chat's `/agents`, and `GET /api/agents`.
// A workspace holds one agent file, so there is one answer; the surfaces differ in how they
// draw it, never in what it is, so it is built here once and each surface only formats it.
//

#pragma once

#include <boost/json.hpp>

#include <string>

namespace pu {

class AgentManager;
struct AgentEntry;

namespace app {

// One agent as a reader needs it: what to type, what it is for, and which product and model
// answer it.
struct AgentSummary {
  std::string name;
  std::string description;
  std::string gateway;
  std::string model;
};

// The configured agent. A workspace with no agent file yields the empty summary, whose name
// is empty -- `pu agents` reports that in words and the API answers with a null object.
// The agent index is passed rather than a `Runtime`: this reads the configuration and nothing
// else, and asking for the whole runtime to reach its lookup would make the formatting layer
// depend on the layer that runs turns.
AgentSummary SummarizeAgent(const AgentManager& agents);

// The shape `GET /api/agents` answers with, and the one `pu agents --json` prints:
// `{name, description, gateway, model}`, or `null` when no agent is configured. One function
// so the two cannot drift into two shapes for one fact.
boost::json::value AgentAsJson(const AgentSummary& agent);

// What a reader sees: the name, its description and the product answering it.
std::string FormatAgent(const AgentSummary& agent);

}  // namespace app
}  // namespace pu
