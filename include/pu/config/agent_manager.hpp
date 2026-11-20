// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// The agents a run can reach, by name.
//
// The agent read from `agents.json`, by name. It is configuration and nothing else — an
// in-memory index over `AgentEntry` — which is why it lives here rather than with the runtime
// that owns it: the answer "which agent is there" is a question about the configuration, and
// the code that formats that answer should not have to reach past it.
//

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

  // The configured entry, or `nullptr` rather than a fabricated one.
  const config::AgentEntry* AgentConfig(const std::string& name) const {
    for (const auto& entry : agent_configs_) {
      if (entry.name == name) return &entry;
    }
    return nullptr;
  }

  // The configured names in configuration order: the order a reader sees, and the same order
  // every time it is asked.
  std::vector<std::string> AgentNames() const {
    std::vector<std::string> names;
    names.reserve(agent_configs_.size());
    for (const auto& entry : agent_configs_) names.push_back(entry.name);
    return names;
  }

  void SetActiveAgent(const std::string& name) { active_agent_ = name; }
  std::string ActiveAgentName() const { return active_agent_; }

 private:
  std::string active_agent_;
  std::vector<config::AgentEntry> agent_configs_;
};

}  // namespace pu
