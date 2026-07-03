// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/backend.hpp"
#include "pu/conversation.hpp"
#include "pu/context.hpp"
#include "pu/stack.hpp"
#include "core/system.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pu::agent {

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

  virtual void OnPanelMessage([[maybe_unused]] const ChatMessage& msg) {}
  virtual std::optional<std::string> ProactiveReply() { return std::nullopt; }
  virtual double EvaluateRelevance([[maybe_unused]] const ChatMessage& msg) { return 0.0; }
  virtual void SetProactiveThreshold([[maybe_unused]] double threshold) {}
};

class AgentManager {
 public:
  AgentManager();

  void RegisterAgent(std::unique_ptr<BaseAgent> agent);
  std::string Dispatch(const std::string& input);
  std::string CallAgent(const std::string& agent_name, const std::string& input);
  void ClearSessions();
  void SetActiveAgent(const std::string& name);
  std::string GetActiveAgent() const;
  BaseAgent* GetAgent(const std::string& name) const;
  void SetShowReasoning(bool enable);
  void SetRecentMessages(const std::vector<ChatMessage>& messages);
  void SetProactiveEnabled(bool enabled);
  void SetProactiveThreshold(double threshold);
  void NotifyPanelMessage(const ChatMessage& msg);
  std::vector<std::pair<std::string, std::string>> CollectProactiveReplies();
  void SetConfirmationCallback(ConfirmationCallback cb);
  void SetSystemPrompt(const std::string& agent_name, const std::string& prompt);
  void SetGlobalContext(std::shared_ptr<GlobalContext> ctx);
  void SetCallStack(std::shared_ptr<CallStack> stack);

  AgentContext PrepareContext(const std::string& agent_name);
  std::string ExecuteAgentWithContext(const std::string& agent_name, const std::string& input, AgentContext& ctx);

  std::unordered_map<std::string, std::vector<ChatMessage>> SnapshotAgents() const;
  void RestoreAgents(const std::unordered_map<std::string, std::vector<ChatMessage>>& states);

 private:
  std::unordered_map<std::string, std::unique_ptr<BaseAgent>> agents_;
  std::string active_agent_;
  bool show_reasoning_ = false;
  bool proactive_enabled_ = false;
  double proactive_threshold_ = 0.6;
  std::vector<ChatMessage> recent_messages_;
  ConfirmationCallback confirmation_callback_;
  std::unordered_map<std::string, std::string> system_prompts_;
  std::shared_ptr<GlobalContext> global_ctx_;
  std::shared_ptr<CallStack> call_stack_;
};

}  // namespace pu::agent
