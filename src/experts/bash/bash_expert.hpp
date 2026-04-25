// SPDX-License-Identifier: GPL-3.0-only
//
// BashExpert: executes safe system commands via native tool calling.

#pragma once

#include "pu/expert.hpp"
#include "pu/backend.hpp"
#include "executor/command_executor.hpp"
#include <memory>
#include <string>

namespace pu::experts {

class BashExpert : public pu::expert::BaseExpert {
 public:
  BashExpert(pu::backend::Backend* backend,
             std::unique_ptr<pu::executor::CommandExecutor> executor);
  ~BashExpert() override = default;

  std::string Name() const override { return "bash"; }
  std::string Description() const override {
    return "Executes safe Linux commands using tool calling. "
           "Performs security review before execution.";
  }

  std::string Handle(const std::string& input, pu::expert::ExpertContext& ctx) override;
  void ResetSession() override;

 private:
  std::string RunToolLoop(const std::string& user_input);

  pu::backend::Backend* backend_;
  std::unique_ptr<pu::executor::CommandExecutor> executor_;
};

}  // namespace pu::experts
