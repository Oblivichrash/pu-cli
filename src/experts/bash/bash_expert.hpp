// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
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
  // backend: non‑owning pointer, must outlive this expert.
  // executor: command execution and safety checks.
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
