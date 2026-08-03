// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "tools/command_executor.hpp"
#include "pu/tools/toolbox.hpp"

#include <memory>
#include <string>

namespace pu::tools {

class ExecuteBashToolStandard : public pu::Tool {
 public:
  explicit ExecuteBashToolStandard(std::unique_ptr<executor::CommandExecutor> executor);
  std::string Name() const override;
  std::string Description() const override;
  std::string ParametersSchema() const override;
  std::string Execute(const nlohmann::json& args, pu::ToolContext& ctx) override;

 private:
  std::unique_ptr<executor::CommandExecutor> executor_;
};

class WriteFileTool : public pu::Tool {
 public:
  std::string Name() const override;
  std::string Description() const override;
  std::string ParametersSchema() const override;
  std::string Execute(const nlohmann::json& args, pu::ToolContext& ctx) override;
};

}  // namespace pu::tools
