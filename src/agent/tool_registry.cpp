// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tool_registry.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace pu::agent {

void ToolRegistry::RegisterTool(std::unique_ptr<Tool> tool) {
  if (!tool) return;
  std::string name = tool->Name();
  if (tools_.find(name) != tools_.end()) {
    throw std::runtime_error("Tool already registered: " + name);
  }
  tools_[name] = std::move(tool);
}

Tool* ToolRegistry::GetTool(const std::string& name) const {
  auto it = tools_.find(name);
  if (it == tools_.end()) return nullptr;
  return it->second.get();
}

std::vector<backend::ToolDefinition> ToolRegistry::GetToolDefinitions() const {
  std::vector<backend::ToolDefinition> defs;
  for (const auto& [name, tool] : tools_) {
    backend::ToolDefinition def;
    def.name = tool->Name();
    def.description = tool->Description();
    def.parameters.raw_schema = tool->ParametersSchema();
    defs.push_back(def);
  }
  return defs;
}

std::string ToolRegistry::ExecuteTool(const std::string& name,
                                      const nlohmann::json& args,
                                      ToolContext& ctx) {
  auto* tool = GetTool(name);
  if (!tool) {
    return "Tool not found: " + name;
  }
  return tool->Execute(args, ctx);
}

}  // namespace pu::agent
