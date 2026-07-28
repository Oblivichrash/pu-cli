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
#include "pu/backend.hpp"
#include "pu/tools/toolbox.hpp"

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

namespace pu::agent {

namespace config {

enum class BackendType { kOllama, kOpenAI };
enum class ToolCallStyle { kDefault, kOpenAI, kPhi4 };

struct SecurityPolicy {
  std::string sandbox_root;
  std::vector<std::string> allowed_paths;
  size_t max_command_length = 0;
  std::vector<std::string> forbidden_patterns;
};

struct BackendConfig {
  BackendType type = BackendType::kOllama;
  std::string host;
  std::string model;
  std::optional<std::string> api_key;
  float temperature = 0.7f;
  std::optional<std::string> system_prompt;
  ToolCallStyle tool_call_style = ToolCallStyle::kDefault;
  bool parameters_as_string = false;
  int max_tokens = 2048;
};

struct AgentEntry {
  std::string name;
  std::string description;
  BackendConfig backend;
  std::vector<std::string> tools;
  SecurityPolicy security;
};

struct AgentsConfig {
  std::string default_agent;
  std::vector<AgentEntry> agents;
};

std::string FindConfigPath();
AgentsConfig LoadAgentsConfig(const std::string& config_path);
void SaveAgentsConfig(const std::string& config_path, const AgentsConfig& cfg);
std::unique_ptr<LLMProvider> CreateBackend(
    const BackendConfig& cfg, std::unique_ptr<pu::http::HttpClient> http);

}  // namespace config

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

  // Show reasoning (for LLM)
  bool GetShowReasoning() const { return show_reasoning_; }
  void SetShowReasoning(bool enable) { show_reasoning_ = enable; }

 private:
  std::string active_agent_;
  bool show_reasoning_ = false;
  ConfirmationCallback confirmation_callback_;

  // Agent config metadata (from agents.json)
  std::vector<config::AgentEntry> agent_configs_;
};

}  // namespace pu::agent