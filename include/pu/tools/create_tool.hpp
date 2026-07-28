// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/tools/tool.hpp"

#include <memory>

namespace pu::tools {

class CreateTool : public pu::Tool {
 public:
  std::string Name() const override;
  std::string Description() const override;
  std::string ParametersSchema() const override;
  std::string Execute(const nlohmann::json& args, pu::ToolContext& ctx) override;
};

}  // namespace pu::tools