// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pu/backend.hpp"
#include "pu/conversation.hpp"
#include "pu/context.hpp"
#include "pu/stack.hpp"
#include "pu/http/http_client.hpp"
#include "executor/command_executor.hpp"

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
std::unique_ptr<backend::Backend> CreateBackend(
    const BackendConfig& cfg, std::unique_ptr<pu::http::HttpClient> http);

}  // namespace config

struct ToolContext {
  const config::SecurityPolicy* security = nullptr;
  std::function<bool(const std::string& message)> request_confirmation;
};

class Tool {
 public:
  virtual ~Tool() = default;
  virtual std::string Name() const = 0;
  virtual std::string Description() const = 0;
  virtual std::string ParametersSchema() const = 0;
  virtual std::string Execute(const nlohmann::json& args, ToolContext& ctx) = 0;
};

class ToolRegistry {
 public:
  void RegisterTool(std::unique_ptr<Tool> tool);
  void RemoveTool(const std::string& name);
  Tool* GetTool(const std::string& name) const;
  std::vector<backend::ToolDefinition> GetToolDefinitions() const;
  std::string ExecuteTool(const std::string& name,
                          const nlohmann::json& args,
                          ToolContext& ctx);
  void ReloadExternalTools(const std::string& directory);

 private:
  std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
  std::unordered_map<std::string, std::string> tool_file_mtimes_;
};

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

struct PendingAction {
  enum class Type { kNone, kPush, kPop };
  Type type = Type::kNone;
  std::string agent_name;
  std::string input_path;
};

struct AgentContext {
  std::function<std::string(const std::string&, const std::string&)> call_expert;
  ConfirmationCallback request_confirmation;
  std::string working_dir;
  bool show_reasoning = false;
  std::vector<ChatMessage> recent_panel_messages;
  std::optional<std::string> system_prompt;
  std::shared_ptr<GlobalContext> global_ctx;
  std::shared_ptr<CallStack> call_stack;
  PendingAction pending_action;
};

class BaseAgent {
 public:
  virtual ~BaseAgent() = default;

  virtual std::string Name() const = 0;
  virtual std::string Description() const = 0;

  virtual std::string Handle(const std::string& input, AgentContext& ctx) = 0;
  virtual void ResetSession() = 0;

  virtual std::vector<ChatMessage> SaveState() const { return {}; }
  virtual void LoadState([[maybe_unused]] const std::vector<ChatMessage>& messages) {}

  [[deprecated("Proactive features removed")]]
  virtual void OnPanelMessage([[maybe_unused]] const ChatMessage& msg) {}
  [[deprecated("Proactive features removed")]]
  virtual std::optional<std::string> ProactiveReply() { return std::nullopt; }
  [[deprecated("Proactive features removed")]]
  virtual double EvaluateRelevance([[maybe_unused]] const ChatMessage& msg) { return 0.0; }
  [[deprecated("Proactive features removed")]]
  virtual void SetProactiveThreshold([[maybe_unused]] double threshold) {}
};

class AgentRegistry {
 public:
  static AgentRegistry& Instance();
  std::unique_ptr<BaseAgent> CreateAgent(const config::AgentEntry& entry);

 private:
  AgentRegistry() = default;
};

class AgentManager {
 public:
  AgentManager();

  void RegisterAgent(std::unique_ptr<BaseAgent> agent);
  BaseAgent* GetAgent(const std::string& name) const;
  std::vector<std::string> GetAgentNames() const;

  void SetActiveAgent(const std::string& name);
  std::string GetActiveAgent() const;

  ConfirmationCallback GetConfirmationCallback() const { return confirmation_callback_; }
  bool GetShowReasoning() const { return show_reasoning_; }
  std::shared_ptr<GlobalContext> GetGlobalContext() const { return global_ctx_; }
  std::shared_ptr<CallStack> GetCallStack() const { return call_stack_; }
  std::optional<std::string> GetSystemPrompt(const std::string& agent_name) const;

  void SetConfirmationCallback(ConfirmationCallback cb);
  void SetShowReasoning(bool enable);
  void SetSystemPrompt(const std::string& agent_name, const std::string& prompt);
  void SetGlobalContext(std::shared_ptr<GlobalContext> ctx);
  void SetCallStack(std::shared_ptr<CallStack> stack);

  void ClearSessions();
  std::unordered_map<std::string, std::vector<ChatMessage>> SnapshotAgents() const;
  void RestoreAgents(const std::unordered_map<std::string, std::vector<ChatMessage>>& states);

 private:
  std::unordered_map<std::string, std::unique_ptr<BaseAgent>> agents_;
  std::string active_agent_;
  bool show_reasoning_ = false;
  ConfirmationCallback confirmation_callback_;
  std::unordered_map<std::string, std::string> system_prompts_;
  std::shared_ptr<GlobalContext> global_ctx_;
  std::shared_ptr<CallStack> call_stack_;
};

}  // namespace pu::agent
