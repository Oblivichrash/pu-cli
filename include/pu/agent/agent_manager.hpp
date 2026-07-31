// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/llm/llm_provider.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "pu/conversation.hpp"
#include "pu/agent_config.hpp"

namespace pu {
class Workspace;
class ForkMergeService;
namespace http {
class HttpClient;
}
namespace executor {
enum class RiskLevel : int;
}
}  // namespace pu

namespace pu {

enum class ConfirmationChoice {
  kDeny,
  kApproveOnce,
  kApproveAllSafe,
  kDenyAll
};

struct ConfirmationRequest {
  std::string description;
  executor::RiskLevel highest_risk;
};

using ConfirmationCallback = std::function<ConfirmationChoice(const ConfirmationRequest&)>;

class AgentManager {
 public:
  AgentManager();

  // Load agent config metadata (for backend switching, listing, etc.)
  void LoadAgentConfigs(const std::vector<config::AgentEntry>& configs);

  // Look up an agent config by name
  const config::AgentEntry* GetAgentConfig(const std::string& name) const;

  // List all agent names from config
  std::vector<std::string> GetAgentNames() const;

  // Active agent name
  void SetActiveAgent(const std::string& name);
  std::string GetActiveAgent() const;

  // Confirmation callback (forward to tools)
  ConfirmationCallback GetConfirmationCallback() const { return confirmation_callback_; }
  void SetConfirmationCallback(ConfirmationCallback cb) { confirmation_callback_ = std::move(cb); }

 private:
  std::string active_agent_;
  ConfirmationCallback confirmation_callback_;

  // Agent config metadata (from agents.json)
  std::vector<config::AgentEntry> agent_configs_;
};

}  // namespace pu
