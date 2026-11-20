// SPDX-License-Identifier: GPL-3.0-only
// The one rule a session-free tool follows to pick a gateway. Two binaries read a provider
// without a session, and a second copy of this rule is a second answer to "which gateway",
// which is the kind of drift that only shows up when the two disagree about a config file.

#include "pu/config/backend_from_agent.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace pu::config {

namespace {

// The entry a file names, with the message a caller needs: the name it does have, because a
// typo or a stale `--agent` is the only reason this branch runs in practice.
const AgentEntry& FindAgent(const AgentEntry& entry, const std::string& wanted) {
  if (entry.name != wanted) {
    throw std::runtime_error("agent is not configured: " + wanted + "; configured: " + entry.name);
  }
  return entry;
}

}  // namespace

model::Backend BackendFromAgent(const std::string& gateway, const std::string& model,
                                const std::string& agent, const AgentEntry& entry) {
  if (!gateway.empty()) {
    model::Backend backend = model::BackendForGateway(gateway);
    if (!model.empty()) backend.model = model;
    return backend;
  }

  const std::string wanted = agent.empty() ? entry.name : agent;
  // The entry's backend already carries the agent's model, so only an explicit `model`
  // overrides it, the way it does for a named gateway.
  model::Backend backend = FindAgent(entry, wanted).backend;
  if (!model.empty()) backend.model = model;
  return backend;
}

model::Backend BackendFromAgent(const std::string& gateway, const std::string& model,
                                const std::string& agent, const std::string& config_path) {
  return BackendFromAgent(gateway, model, agent, LoadAgentConfig(config_path));
}

}  // namespace pu::config
