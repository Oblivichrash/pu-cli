// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/backend.hpp"
#include "pu/conversation.hpp"
#include "pu/proactive_engine.hpp"
#include "executor/command_executor.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pu::expert {

enum class ConfirmationChoice { kDeny, kApproveOnce, kApproveAllSafe, kDenyAll };

struct ConfirmationRequest {
  std::string description;
  executor::RiskLevel highest_risk;
};

using ConfirmationCallback = std::function<ConfirmationChoice(const ConfirmationRequest&)>;

struct ExpertContext {
  std::function<std::string(const std::string&, const std::string&)> call_expert;
  ConfirmationCallback request_confirmation;
  std::string working_dir;
  bool show_reasoning = false;
  std::vector<ChatMessage> recent_panel_messages;
};

class BaseExpert {
 public:
  virtual ~BaseExpert() = default;
  virtual std::string Name() const = 0;
  virtual std::string Description() const = 0;
  virtual std::string Handle(const std::string& input, ExpertContext& ctx) = 0;
  virtual void ResetSession() = 0;

  virtual std::vector<ChatMessage> SaveState() const { return {}; }
  virtual void LoadState([[maybe_unused]] const std::vector<ChatMessage>& messages) {}
  virtual void OnPanelMessage([[maybe_unused]] const ChatMessage& msg) {}
  virtual std::optional<std::string> ProactiveReply() { return std::nullopt; }
  virtual double EvaluateRelevance([[maybe_unused]] const ChatMessage& msg) { return 0.0; }
  virtual void SetProactiveThreshold([[maybe_unused]] double threshold) {}
};

class ExpertManager {
 public:
  ExpertManager();
  void RegisterExpert(std::unique_ptr<BaseExpert> expert);
  std::string Dispatch(const std::string& input);
  std::string CallExpert(const std::string& expert_name, const std::string& input);
  void ClearSessions();
  void SetActiveExpert(const std::string& name);
  std::string GetActiveExpert() const;
  void SetShowReasoning(bool enable);
  void SetRecentMessages(const std::vector<ChatMessage>& messages);
  void SetProactiveEnabled(bool enabled);
  void SetProactiveThreshold(double threshold);
  void NotifyPanelMessage(const ChatMessage& msg);
  std::vector<std::pair<std::string, std::string>> CollectProactiveReplies();
  void SetConfirmationCallback(ConfirmationCallback cb);

  std::unordered_map<std::string, std::vector<ChatMessage>> SnapshotExperts() const;
  void RestoreExperts(const std::unordered_map<std::string, std::vector<ChatMessage>>& states);

 private:
  std::unordered_map<std::string, std::unique_ptr<BaseExpert>> experts_;
  std::string active_expert_;
  bool show_reasoning_ = false;
  std::unique_ptr<ProactiveEngine> proactive_engine_;
  ConfirmationCallback confirmation_callback_;
};

}  // namespace pu::expert
