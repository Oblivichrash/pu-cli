// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/llm/llm_provider.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pu/agent_config.hpp"

namespace pu {
class Workspace;
namespace http {
class HttpClient;
}
namespace executor {
enum class RiskLevel { kSafe, kNeutral, kDangerous };
}
}  // namespace pu

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
