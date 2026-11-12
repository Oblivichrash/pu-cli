// SPDX-License-Identifier: GPL-3.0-only

//
// The agents a workspace offers, in one place.
//
// Four surfaces ask this question: `pu agents`, the chat's `/agents`, `GET /api/agents`, and
// the tooltip the Web panel shows on the selector. They differ in how they draw the answer,
// never in what it is, so the answer is built here once and each surface only formats it.
//

#pragma once

#include <boost/json.hpp>

#include <string>
#include <vector>

namespace pu {

class AgentManager;

namespace config {

// One agent as a reader needs it: what to type, what it is for, which product answers it, and
// which model. A surface that wants less leaves fields out of what it prints; none of them
// has to work them out again.
struct AgentSummary {
  std::string name;
  std::string description;
  std::string gateway;
  std::string model;
  // What this one may hand a turn to. The list is the whole of what it may reach, so a reader
  // looking at who can call whom reads it here rather than in each table.
  std::vector<std::string> delegates;
  bool active = false;
};

// Every configured agent, in configuration order, with `active` set on the one the session
// names. The agent index is passed rather than a `Runtime`: this reads the configuration and
// nothing else, and asking for the whole runtime to reach two of its lookups would make the
// formatting layer depend on the layer that runs turns.
std::vector<AgentSummary> SummarizeAgents(const AgentManager& agents, const std::string& active);

// The shape `GET /api/agents` answers with, and the one `pu agents --json` prints: a JSON
// array of `{name, description, gateway, model, active}`. One function so the two cannot
// drift into two shapes for one fact.
boost::json::value AgentsAsJson(const std::vector<AgentSummary>& agents);

// The table a reader sees: the name, a marker on the active one, and its description.
std::string FormatAgentTable(const std::vector<AgentSummary>& agents);

}  // namespace config
}  // namespace pu
