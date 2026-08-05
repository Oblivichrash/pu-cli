// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/tools/toolbox.hpp"

#include <string>

namespace pu::tools {

class ExecuteBashToolStandard : public pu::Tool {
 public:
  explicit ExecuteBashToolStandard(std::string sandbox_root);
  std::string Name() const override;
  std::string Description() const override;
  std::string ParametersSchema() const override;
  std::string Execute(const nlohmann::json& args, pu::ToolContext& ctx) override;

 private:
  std::string sandbox_root_;
};

class WriteFileTool : public pu::Tool {
 public:
  std::string Name() const override;
  std::string Description() const override;
  std::string ParametersSchema() const override;
  std::string Execute(const nlohmann::json& args, pu::ToolContext& ctx) override;
};

}  // namespace pu::tools
