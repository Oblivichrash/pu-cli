// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "executor/command_executor.hpp"
#include "pu/agent_core.hpp"

#include <memory>

namespace pu::tools {

class ExecuteBashToolStandard : public agent::Tool {
 public:
  explicit ExecuteBashToolStandard(std::unique_ptr<executor::CommandExecutor> executor);
  std::string Name() const override;
  std::string Description() const override;
  std::string ParametersSchema() const override;
  std::string Execute(const nlohmann::json& args, agent::ToolContext& ctx) override;

 private:
  std::unique_ptr<executor::CommandExecutor> executor_;
};

}  // namespace pu::tools
