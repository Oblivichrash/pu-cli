// SPDX-License-Identifier: GPL-3.0-only
#include "pu/agent_core.hpp"

#include "pu/error.hpp"
#include "pu/tools/python_tool.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace pu::agent {

void ToolRegistry::RegisterTool(std::unique_ptr<Tool> tool) {
  if (!tool) return;
  std::string name = tool->Name();
  if (tools_.find(name) != tools_.end()) {
    throw pu::Error("Tool already registered: " + name);
  }
  tools_[name] = std::move(tool);
}

void ToolRegistry::RemoveTool(const std::string& name) {
  tools_.erase(name);
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
  Tool* tool = GetTool(name);
  if (!tool) {
    return "Tool not found: " + name;
  }
  return tool->Execute(args, ctx);
}

void ToolRegistry::ReloadExternalTools(const std::string& directory) {
  namespace fs = std::filesystem;
  if (!fs::exists(directory) || !fs::is_directory(directory)) {
    return;
  }

  std::unordered_map<std::string, std::string> current_files;
  for (const auto& entry : fs::directory_iterator(directory)) {
    if (entry.path().extension() == ".py") {
      std::string tool_name = entry.path().stem().string();
      auto mod_time = fs::last_write_time(entry);
      std::string time_str = std::to_string(static_cast<unsigned long long>(mod_time.time_since_epoch().count()));
      current_files[tool_name] = time_str;
    }
  }

  for (auto& [name, mtime] : current_files) {
    auto it = tool_file_mtimes_.find(name);
    if (it == tool_file_mtimes_.end() || it->second != mtime) {
      try {
        auto tool = std::make_unique<tools::PythonTool>((fs::path(directory) / (name + ".py")).string());
        RemoveTool(name);
        RegisterTool(std::move(tool));
        tool_file_mtimes_[name] = mtime;
      } catch (const std::exception& e) {
        std::cerr << "[ToolRegistry] Failed to load tool " << name << ": " << e.what() << '\n';
      }
    }
  }

  for (auto it = tool_file_mtimes_.begin(); it != tool_file_mtimes_.end(); ) {
    if (current_files.find(it->first) == current_files.end()) {
      RemoveTool(it->first);
      it = tool_file_mtimes_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace pu::agent
