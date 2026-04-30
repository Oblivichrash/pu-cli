// SPDX-License-Identifier: GPL-3.0-only
//
// BashExpert: executes safe system commands via native tool calling.

#pragma once

#include "pu/expert.hpp"
#include "pu/backend.hpp"
#include "pu/conversation.hpp"
#include "executor/command_executor.hpp"

#include <memory>
#include <string>
#include <vector>

namespace pu::experts {

class BashExpert : public pu::expert::BaseExpert {
 public:
  BashExpert(const std::string& name,
             std::unique_ptr<pu::backend::Backend> backend,
             std::unique_ptr<pu::executor::CommandExecutor> executor);
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

 private:
  std::string RunToolLoop(const std::string& user_input,
                          bool show_reasoning,
                          std::vector<ChatMessage>& turn_history,
                          const std::vector<pu::backend::Message>& initial_history = {});

  std::string name_;
  std::unique_ptr<pu::backend::Backend> backend_;
  std::unique_ptr<pu::executor::CommandExecutor> executor_;
  std::vector<ChatMessage> history_;
};

}  // namespace pu::experts
