// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/tool.hpp"
#include <memory>
#include <unordered_map>
#include <vector>

namespace pu::agent {

class ToolRegistry {
 public:
  void RegisterTool(std::unique_ptr<Tool> tool);
  Tool* GetTool(const std::string& name) const;
  std::vector<backend::ToolDefinition> GetToolDefinitions() const;
  std::string ExecuteTool(const std::string& name,
                          const nlohmann::json& args,
                          ToolContext& ctx);

 private:
  std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
};

}  // namespace pu::agent
