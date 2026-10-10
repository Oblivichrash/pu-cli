// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/toolbox.hpp"

#include "pu/core/base.hpp"

#include <spdlog/spdlog.h>

#include <cctype>
#include <filesystem>
#include <iostream>

namespace pu {

namespace {

bool IsAllowedToolNameChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}

}  // namespace

std::string Toolbox::SanitizeToolName(const std::string& name) {
  std::string result;
  result.reserve(name.size());
  for (char c : name) {
    if (IsAllowedToolNameChar(c)) {
      result.push_back(c);
    } else {
      result.push_back('_');  // replace non-compliant chars with '_'
    }
  }
  return result;
}

void Toolbox::RegisterTool(std::unique_ptr<Tool> tool) {
  if (!tool) return;
  const std::string original_name = tool->Name();
  if (original_name.empty()) {
    throw pu::Error("Tool name cannot be empty");
  }

  std::string display_name = SanitizeToolName(original_name);

  if (tools_.find(display_name) != tools_.end()) {
    const std::string base = display_name;
    int suffix = 1;
    while (tools_.find(display_name) != tools_.end()) {
      display_name = base + "_" + std::to_string(suffix++);
    }
  }

  tool->display_name_ = display_name;
  tools_[display_name] = std::move(tool);
}

std::vector<ToolDefinition> Toolbox::GetToolDefinitions() const {
  std::vector<ToolDefinition> defs;
  defs.reserve(tools_.size());
  for (const auto& [display_name, tool] : tools_) {
    ToolDefinition def;
    def.name = display_name;  // LLM sees sanitized name
    def.description = tool->Description();
    def.parameters = tool->ParametersSchema();
    defs.push_back(std::move(def));
  }
  return defs;
}

std::string Toolbox::ExecuteTool(const std::string& name, const boost::json::value& args,
                                 ToolContext& ctx) {
  const auto it = tools_.find(name);
  if (it == tools_.end()) {
    spdlog::warn("Tool not found: {}", name);
    return "Tool not found: " + name;
  }

  return it->second->Execute(args, ctx);
}

}  // namespace pu
