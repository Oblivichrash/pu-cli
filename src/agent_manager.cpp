// SPDX-License-Identifier: GPL-3.0-only
#include "pu/agent_manager.hpp"

#include <iostream>

namespace pu {

AgentManager::AgentManager() {}

void AgentManager::LoadAgentConfigs(const std::vector<config::AgentEntry>& configs) {
  agent_configs_ = configs;
}

const config::AgentEntry* AgentManager::GetAgentConfig(const std::string& name) const {
  for (const auto& entry : agent_configs_) {
    if (entry.name == name) return &entry;
  }
  return nullptr;
}

std::vector<std::string> AgentManager::GetAgentNames() const {
  std::vector<std::string> names;
  for (const auto& entry : agent_configs_) {
    names.push_back(entry.name);
  }
  return names;
}

void AgentManager::SetActiveAgent(const std::string& name) {
  active_agent_ = name;
}

std::string AgentManager::GetActiveAgent() const {
  return active_agent_;
}

}  // namespace pu