// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/tools/tool.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include "pu/llm/llm_provider.hpp"

namespace pu {

class Toolbox {
 public:
  void RegisterTool(std::unique_ptr<Tool> tool);
  void RemoveTool(const std::string& name);
  Tool* GetTool(const std::string& name) const;
  std::vector<ToolDefinition> GetToolDefinitions() const;
  std::string ExecuteTool(const std::string& name,
                          const nlohmann::json& args,
                          ToolContext& ctx);

 private:
  std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
};

}  // namespace pu
