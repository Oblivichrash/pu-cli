// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// BashExpert: executes safe system commands on behalf of the user.

#pragma once

#include "executor/command_executor.hpp"
#include "pu/expert.hpp"
#include "pu/backend.hpp"
#include <memory>
#include <string>

namespace pu::experts {

class BashExpert : public pu::expert::BaseExpert {
 public:
  // cmd_backend is a non-owning pointer used for command generation.
  BashExpert(pu::backend::Backend* cmd_backend);
  ~BashExpert() override = default;

  std::string Name() const override { return "bash"; }
  std::string Description() const override {
    return "Executes safe Linux commands. Can list files, create directories, "
           "run scripts, etc. Requires user confirmation for destructive actions.";
  }

  std::string Handle(const std::string& input, pu::expert::ExpertContext& ctx) override;
  void ResetSession() override;

 private:
  std::string GenerateCommand(const std::string& task);
  std::string ExecuteCommand(const std::string& command);

  std::unique_ptr<pu::executor::CommandExecutor> executor_;
  pu::backend::Backend* cmd_backend_;  // borrowed, not owned
};

}  // namespace pu::experts
