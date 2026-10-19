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

class Runtime;

namespace config {

// One agent as a reader needs it: what to type, what it is for, which product answers it, and
// which model. A surface that wants less leaves fields out of what it prints; none of them
// has to work them out again.
struct AgentSummary {
  std::string name;
  std::string description;
  std::string gateway;
  std::string model;
  bool active = false;
};

// Every configured agent, in configuration order, with `active` set on the one the session
// names. The session's answer is passed rather than read: the chat and the CLI learn it from
// different places, and this is about the list, not about who is asking.
std::vector<AgentSummary> SummarizeAgents(const Runtime& runtime, const std::string& active);

// The shape `GET /api/agents` answers with, and the one `pu agents --json` prints: a JSON
// array of `{name, description, gateway, model, active}`. One function so the two cannot
// drift into two shapes for one fact.
boost::json::value AgentsAsJson(const std::vector<AgentSummary>& agents);

// The table a reader sees: the name, a marker on the active one, and its description.
std::string FormatAgentTable(const std::vector<AgentSummary>& agents);

}  // namespace config
}  // namespace pu
