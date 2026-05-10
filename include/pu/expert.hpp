// SPDX-License-Identifier: GPL-3.0-only
//
// Expert framework base classes.

#pragma once

#include "pu/backend.hpp"
#include "pu/conversation.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pu::expert {

struct ExpertContext {
  std::function<std::string(const std::string& name, const std::string& input)> call_expert;
  std::function<bool(const std::string& message)> request_confirmation;
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

  std::unordered_map<std::string, std::vector<ChatMessage>> SnapshotExperts() const;
  void RestoreExperts(const std::unordered_map<std::string, std::vector<ChatMessage>>& states);

 private:
  std::unordered_map<std::string, std::unique_ptr<BaseExpert>> experts_;
  std::string active_expert_;
  bool show_reasoning_ = false;
  std::vector<ChatMessage> recent_messages_;
  bool proactive_enabled_ = false;
  double proactive_threshold_ = 0.6;
};

}  // namespace pu::expert
