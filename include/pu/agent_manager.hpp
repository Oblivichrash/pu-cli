// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

#include "pu/config/agents.hpp"

namespace pu {

class AgentManager {
 public:
  AgentManager() = default;

  void LoadAgentConfigs(const std::vector<config::AgentEntry>& configs) {
    agent_configs_ = configs;
  }

  const config::AgentEntry* GetAgentConfig(const std::string& name) const {
    for (const auto& entry : agent_configs_) {
      if (entry.name == name) return &entry;
    }
    return nullptr;
  }

  std::vector<std::string> GetAgentNames() const {
    std::vector<std::string> names;
    names.reserve(agent_configs_.size());
    for (const auto& entry : agent_configs_) names.push_back(entry.name);
    return names;
  }

  void SetActiveAgent(const std::string& name) { active_agent_ = name; }
  std::string GetActiveAgent() const { return active_agent_; }

 private:
  std::string active_agent_;
  std::vector<config::AgentEntry> agent_configs_;
};

}  // namespace pu
