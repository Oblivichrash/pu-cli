// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/agent_core.hpp"

#include <string>

namespace pu::tools {

class WriteFileTool : public agent::Tool {
 public:
  std::string Name() const override;
  std::string Description() const override;
  std::string ParametersSchema() const override;
  std::string Execute(const nlohmann::json& args, agent::ToolContext& ctx) override;
};

}  // namespace pu::tools
