// SPDX-License-Identifier: GPL-3.0-only
// The one rule a session-free tool follows to pick a gateway. Two binaries read a provider
// without a session, and a second copy of this rule is a second answer to "which gateway",
// which is the kind of drift that only shows up when the two disagree about a config file.

#include "pu/config/backend_from_table.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace pu::config {

namespace {

// The agent entry the table names, with the message a caller needs: which names it could have
// used, because a typo is the only reason this branch runs in practice.
const AgentEntry& FindAgent(const AgentsConfig& agents, const std::string& wanted) {
  const auto entry = std::find_if(
      agents.agents.begin(), agents.agents.end(),
      [&](const AgentEntry& candidate) { return candidate.name == wanted; });
  if (entry == agents.agents.end()) {
    std::string known;
    for (const AgentEntry& candidate : agents.agents) known += " " + candidate.name;
    throw std::runtime_error(
        "agent is not configured: " + wanted +
        (known.empty() ? " (no agents are configured)" : "; configured:" + known));
  }
  return *entry;
}

}  // namespace

model::Backend BackendFromTable(const std::string& gateway, const std::string& model,
                                const std::string& agent, const AgentsConfig& agents) {
  if (!gateway.empty()) {
    model::Backend backend = model::BackendForGateway(gateway);
    if (!model.empty()) backend.model = model;
    return backend;
  }

  const std::string wanted = agent.empty() ? agents.default_agent : agent;
  // The entry's backend already carries the agent's model, so only an explicit `model`
  // overrides it, the way it does for a named gateway.
  model::Backend backend = FindAgent(agents, wanted).backend;
  if (!model.empty()) backend.model = model;
  return backend;
}

model::Backend BackendFromTable(const std::string& gateway, const std::string& model,
                                const std::string& agent, const std::string& config_path) {
  return BackendFromTable(gateway, model, agent, LoadAgentsConfig(config_path));
}

}  // namespace pu::config
