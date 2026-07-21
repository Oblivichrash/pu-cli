// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/agent_core.hpp"
#include "pu/backend.hpp"
#include "pu/conversation.hpp"
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
           const agent::config::SecurityPolicy& security = {});

  std::string Name() const override { return name_; }
  std::string Description() const override { return description_; }
  void SetDescription(const std::string& desc) { description_ = desc; }

  std::string Handle(const std::string& input, agent::AgentContext& ctx) override;
  void ResetSession() override;

  std::vector<ChatMessage> SaveState() const override;
  void LoadState(const std::vector<ChatMessage>& messages) override;

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
  agent::config::SecurityPolicy security_;
  std::vector<ChatMessage> history_;
};

}  // namespace pu::agents
