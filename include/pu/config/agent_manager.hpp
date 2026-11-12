// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// The agents a run can reach, by name.
//
// The table read from `agents.json` and the ones adopted mid-run from a gateway share this one
// list, so a lookup does not have to know which of the two a name came from. It is configuration
// and nothing else — an in-memory index over `AgentEntry` — which is why it lives here rather
// than with the runtime that owns it: the answer "which agents are there" is a question about
// the configuration, and the code that formats that answer should not have to reach past it.
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

  // The table's entry first, then any adopted one: a name is answered by whichever list holds
  // it, and a name in neither is `nullptr` rather than a fabricated entry.
  const config::AgentEntry* AgentConfig(const std::string& name) const {
    for (const auto& entry : agent_configs_) {
      if (entry.name == name) return &entry;
    }
    for (const auto& entry : transient_agents_) {
      if (entry.name == name) return &entry;
    }
    return nullptr;
  }

  // Take an agent learned at runtime, replacing one of the same name so a re-read of a gateway
  // does not grow the list with duplicates. The reference is to the stored copy, and stays
  // valid until the next `Adopt`.
  const config::AgentEntry& Adopt(const config::AgentEntry& entry) {
    for (auto& existing : transient_agents_) {
      if (existing.name == entry.name) {
        existing = entry;
        return existing;
      }
    }
    transient_agents_.push_back(entry);
    return transient_agents_.back();
  }

  // The table's names in configuration order, then the adopted ones: the order a reader sees,
  // and the same order every time it is asked.
  std::vector<std::string> AgentNames() const {
    std::vector<std::string> names;
    names.reserve(agent_configs_.size() + transient_agents_.size());
    for (const auto& entry : agent_configs_) names.push_back(entry.name);
    for (const auto& entry : transient_agents_) names.push_back(entry.name);
    return names;
  }

  void SetActiveAgent(const std::string& name) { active_agent_ = name; }
  std::string ActiveAgentName() const { return active_agent_; }

 private:
  std::string active_agent_;
  std::vector<config::AgentEntry> agent_configs_;
  std::vector<config::AgentEntry> transient_agents_;
};

}  // namespace pu
