// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The configured agents, and which one of them is active.

#include <string>
#include <vector>

#include "pu/config/agents.hpp"

namespace pu {

class AgentManager {
 public:
  AgentManager();

  void LoadAgentConfigs(const std::vector<config::AgentEntry>& configs);

  const config::AgentEntry* GetAgentConfig(const std::string& name) const;

  std::vector<std::string> GetAgentNames() const;

  void SetActiveAgent(const std::string& name);
  std::string GetActiveAgent() const;

 private:
  std::string active_agent_;

  std::vector<config::AgentEntry> agent_configs_;
};

}  // namespace pu
