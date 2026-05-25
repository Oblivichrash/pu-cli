// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/agent.hpp"
#include "pu/backend.hpp"
#include "pu/conversation.hpp"

#include <memory>
#include <string>
#include <vector>

namespace pu::agents {

class ChatAgent : public pu::agent::BaseAgent {
 public:
  explicit ChatAgent(const std::string& name,
                     std::unique_ptr<pu::backend::Backend> backend,
                     const std::string& model_id = "");
  ~ChatAgent() override = default;

  std::string Name() const override { return name_; }
  std::string Description() const override {
    return model_id_.empty() ? "General conversational assistant"
                             : "General conversational assistant powered by " + model_id_;
  }

  std::string Handle(const std::string& input, pu::agent::AgentContext& ctx) override;
  void ResetSession() override;

  std::vector<pu::ChatMessage> SaveState() const override;
  void LoadState(const std::vector<pu::ChatMessage>& messages) override;

 private:
  std::string name_;
  std::string model_id_;
  std::unique_ptr<pu::backend::Backend> backend_;
  std::vector<pu::ChatMessage> history_;
};

}  // namespace pu::agents
