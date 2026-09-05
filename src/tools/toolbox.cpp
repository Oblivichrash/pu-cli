// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/toolbox.hpp"

#include "pu/error.hpp"

#include <spdlog/spdlog.h>

#include <cctype>
#include <filesystem>
#include <iostream>

namespace pu {

namespace {

// Check if a character is allowed in LLM tool names (alnum, '_', '-').
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
      result.push_back('_');  // replace non‑compliant chars with '_'
    }
  }
  return result;
}

void Toolbox::RegisterTool(std::unique_ptr<Tool> tool) {
  if (!tool) return;
  std::string original_name = tool->Name();
  if (original_name.empty()) {
    throw pu::Error("Tool name cannot be empty");
  }

  // Generate a sanitized (LLM‑friendly) display name.
  std::string display_name = SanitizeToolName(original_name);

  // If the sanitized name already exists, append a counter suffix.
  if (tools_.find(display_name) != tools_.end()) {
    int suffix = 1;
    std::string base = display_name;
    while (tools_.find(display_name) != tools_.end()) {
      display_name = base + "_" + std::to_string(suffix++);
    }
  }

  tools_[display_name] = std::move(tool);
  display_to_original_[display_name] = original_name;
}

void Toolbox::RemoveTool(const std::string& name) {
  // name here is the display name (sanitized) used as key.
  auto it = tools_.find(name);
  if (it != tools_.end()) {
    display_to_original_.erase(name);
    tools_.erase(it);
  }
}

Tool* Toolbox::GetTool(const std::string& name) const {
  // name is the display name (sanitized) from LLM.
  auto it = tools_.find(name);
  if (it == tools_.end()) return nullptr;
  return it->second.get();
}

std::vector<ToolDefinition> Toolbox::GetToolDefinitions() const {
  std::vector<ToolDefinition> defs;
  for (const auto& [display_name, tool] : tools_) {
    ToolDefinition def;
    def.name = display_name;                    // LLM sees sanitized name
    def.description = tool->Description();
    def.parameters_schema = tool->ParametersSchema();
    defs.push_back(def);
  }
  return defs;
}

std::string Toolbox::ExecuteTool(const std::string& name, const boost::json::value& args, ToolContext& ctx) {
  if (name.empty()) {
    spdlog::error("Attempted to execute tool with empty name");
    return "Error: tool name is empty";
  }

  // Look up the original name via the mapping.
  auto orig_it = display_to_original_.find(name);
  if (orig_it == display_to_original_.end()) {
    spdlog::error("Tool not found: {}", name);
    return "Tool not found: " + name;
  }

  // Retrieve the tool by the same display name (it's the key in tools_).
  auto tool_it = tools_.find(name);
  if (tool_it == tools_.end()) {
    spdlog::error("Tool not found in tools_ map: {}", name);
    return "Tool not found: " + name;
  }

  return tool_it->second->Execute(args, ctx);
}

}  // namespace pu
