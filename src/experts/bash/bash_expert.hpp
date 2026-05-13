// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/expert.hpp"
#include "pu/backend.hpp"
#include "pu/conversation.hpp"
#include "executor/command_executor.hpp"
#include "pu/expert_config.hpp"
#include <memory>
#include <string>
#include <vector>

namespace pu::experts {

class BashExpert : public pu::expert::BaseExpert {
 public:
  BashExpert(const std::string& name, std::unique_ptr<pu::backend::Backend> backend,
             std::unique_ptr<pu::executor::CommandExecutor> executor,
             config::ConfirmationPolicy policy = config::ConfirmationPolicy::kAlwaysAsk);
  ~BashExpert() override = default;

  std::string Name() const override { return name_; }
  std::string Description() const override {
    return "Executes safe Linux commands using tool calling. "
           "Performs security review before execution.";
  }
  std::string Handle(const std::string& input, pu::expert::ExpertContext& ctx) override;
  void ResetSession() override;
  std::vector<ChatMessage> SaveState() const override;
  void LoadState(const std::vector<ChatMessage>& messages) override;
  void OnPanelMessage(const ChatMessage& msg) override;
  std::optional<std::string> ProactiveReply() override;
  double EvaluateRelevance(const ChatMessage& msg) override;
  void SetProactiveThreshold(double threshold) override;

 protected:
  std::vector<pu::backend::Message> BuildInitialHistory() const;
  void AppendTurnToHistory(const std::vector<pu::backend::Message>& history,
                           size_t initial_size,
                           std::vector<ChatMessage>& turn_history) const;

 private:
  std::string RunToolLoop(const std::string& user_input, bool show_reasoning,
                          std::vector<ChatMessage>& turn_history,
                          pu::expert::ExpertContext& ctx,
                          const std::vector<pu::backend::Message>& initial_history = {});

  std::string name_;
  std::unique_ptr<pu::backend::Backend> backend_;
  std::unique_ptr<pu::executor::CommandExecutor> executor_;
  std::vector<ChatMessage> history_;
  std::vector<double> recent_scores_;
  double proactive_threshold_ = 0.6;
  config::ConfirmationPolicy confirmation_policy_;
  bool user_approved_all_safe_ = false;
};

}  // namespace pu::experts
