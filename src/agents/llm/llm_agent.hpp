// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/agent.hpp"
#include "pu/backend.hpp"
#include "pu/tool_registry.hpp"
#include "pu/conversation.hpp"
#include "pu/agent_config.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pu::agents {

class LLMAgent : public agent::BaseAgent {
 public:
  LLMAgent(const std::string& name,
           std::unique_ptr<backend::Backend> backend,
           std::unique_ptr<agent::ToolRegistry> tool_registry,
           const config::SecurityPolicy& security = {});

  std::string Name() const override { return name_; }
  std::string Description() const override { return description_; }
  void SetDescription(const std::string& desc) { description_ = desc; }

  std::string Handle(const std::string& input, agent::AgentContext& ctx) override;
  void ResetSession() override;

  std::vector<ChatMessage> SaveState() const override;
  void LoadState(const std::vector<ChatMessage>& messages) override;

  void OnPanelMessage(const ChatMessage& msg) override;
  std::optional<std::string> ProactiveReply() override;
  double EvaluateRelevance(const ChatMessage& msg) override;
  void SetProactiveThreshold(double threshold) override;

  void ReloadExternalTools();

 private:
  std::string RunToolLoop(const std::string& user_input,
                          bool show_reasoning,
                          std::vector<ChatMessage>& turn_history,
                          agent::AgentContext& ctx,
                          const std::vector<backend::Message>& initial_history);

  std::vector<backend::Message> BuildInitialHistory() const;
  void AppendTurnToHistory(const std::vector<backend::Message>& history,
                           size_t initial_size,
                           std::vector<ChatMessage>& turn_history) const;

  std::string name_;
  std::string description_;
  std::unique_ptr<backend::Backend> backend_;
  std::unique_ptr<agent::ToolRegistry> tool_registry_;
  config::SecurityPolicy security_;
  std::vector<ChatMessage> history_;
  std::vector<double> recent_scores_;
  double proactive_threshold_ = 0.6;
};

}  // namespace pu::agents
