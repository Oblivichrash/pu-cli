// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/toolbox.hpp"

#include "pu/error.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <iostream>

namespace pu {

void Toolbox::RegisterTool(std::unique_ptr<Tool> tool) {
  if (!tool) return;
  std::string name = tool->Name();
  // Empty name would cause ambiguous lookup and potential security issues.
  if (name.empty()) {
    throw pu::Error("Tool name cannot be empty");
  }
  if (tools_.find(name) != tools_.end()) {
    throw pu::Error("Tool already registered: " + name);
  }
  tools_[name] = std::move(tool);
}

Tool* Toolbox::GetTool(const std::string& name) const {
  auto it = tools_.find(name);
  if (it == tools_.end()) return nullptr;
  return it->second.get();
}

std::vector<ToolDefinition> Toolbox::GetToolDefinitions() const {
  std::vector<ToolDefinition> defs;
  for (const auto& [name, tool] : tools_) {
    ToolDefinition def;
    def.name = tool->Name();
    def.description = tool->Description();
    def.parameters_schema = tool->ParametersSchema();
    defs.push_back(def);
  }
  return defs;
}

std::string Toolbox::ExecuteTool(const std::string& name,
                                 const nlohmann::json& args,
                                 ToolContext& ctx) {
  // Final safety net: reject empty names even if they bypass earlier checks.
  if (name.empty()) {
    spdlog::error("Attempted to execute tool with empty name");
    return "Error: tool name is empty";
  }
  Tool* tool = GetTool(name);
  if (!tool) {
    return "Tool not found: " + name;
  }
  return tool->Execute(args, ctx);
}

}  // namespace pu
