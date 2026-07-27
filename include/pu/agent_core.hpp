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

class BaseAgent {
 public:
  virtual ~BaseAgent() = default;

  virtual std::string Name() const = 0;
  virtual std::string Description() const = 0;

  virtual std::string Handle(const std::string& input, class AgentContext& ctx) = 0;
  virtual void ResetSession() = 0;

  virtual std::vector<ChatMessage> SaveState() const { return {}; }
  virtual void LoadState([[maybe_unused]] const std::vector<ChatMessage>& messages) {}

  virtual ConfirmationCallback GetConfirmationCallback() const { return nullptr; }
  virtual void SetConfirmationCallback(ConfirmationCallback cb) { (void)cb; }
};

struct PendingAction {
  enum class Type { kNone, kPush, kPop };
  Type type = Type::kNone;
  std::string agent_name;
  std::string input_path;
};

struct AgentContext {
  std::shared_ptr<pu::Workspace> context;
  PendingAction pending_action;
  std::shared_ptr<pu::ForkMergeService> fork_service;
};

class AgentManager {
 public:
  AgentManager();

  void RegisterAgent(std::unique_ptr<BaseAgent> agent);
  BaseAgent* GetAgent(const std::string& name) const;
  std::vector<std::string> GetAgentNames() const;

  // New: Load agent config metadata (for backend switching, listing, etc.)
  void LoadAgentConfigs(const std::vector<config::AgentEntry>& configs);

  // New: Look up an agent config by name
  const config::AgentEntry* GetAgentConfig(const std::string& name) const;

  void SetActiveAgent(const std::string& name);
  std::string GetActiveAgent() const;

  ConfirmationCallback GetConfirmationCallback() const { return confirmation_callback_; }
  bool GetShowReasoning() const { return show_reasoning_; }
  std::optional<std::string> GetSystemPrompt(const std::string& agent_name) const;

  void SetConfirmationCallback(ConfirmationCallback cb);
  void SetShowReasoning(bool enable);
  void SetSystemPrompt(const std::string& agent_name, const std::string& prompt);

  void ClearSessions();
  std::unordered_map<std::string, std::vector<ChatMessage>> SnapshotAgents() const;
  void RestoreAgents(const std::unordered_map<std::string, std::vector<ChatMessage>>& states);

 private:
  std::unordered_map<std::string, std::unique_ptr<BaseAgent>> agents_;
  std::string active_agent_;
  bool show_reasoning_ = false;
  ConfirmationCallback confirmation_callback_;
  std::unordered_map<std::string, std::string> system_prompts_;

  // Agent config metadata (from agents.json)
  std::vector<config::AgentEntry> agent_configs_;
};

}  // namespace pu::agent