// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include "pu/llm/llm_provider.hpp"
#include "pu/agent_config.hpp"

namespace pu {

struct ToolContext {
  const config::SecurityPolicy* security = nullptr;
  std::function<bool(const std::string& message)> request_confirmation;
};

class Tool {
 public:
  virtual ~Tool() = default;
  virtual std::string Name() const = 0;
  virtual std::string Description() const = 0;
  virtual std::string ParametersSchema() const = 0;
  virtual std::string Execute(const nlohmann::json& args, ToolContext& ctx) = 0;
};

class Toolbox {
 public:
  void RegisterTool(std::unique_ptr<Tool> tool);
  std::vector<ToolDefinition> GetToolDefinitions() const;
  std::string ExecuteTool(const std::string& name,
                          const nlohmann::json& args,
                          ToolContext& ctx);

 private:
  // Lookup helper used only by ExecuteTool.
  Tool* GetTool(const std::string& name) const;
  std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
};

}  // namespace pu

namespace pu::tools {

// Canonical structured result for every tool (built-in and MCP); the executor
// stores this JSON verbatim in the transcript so the model sees the outcome.
inline std::string MakeToolResultJson(bool success,
                                      const std::string& stdout_content,
                                      const std::string& stderr_content,
                                      const std::string& error,
                                      int exit_code) {
  nlohmann::json j;
  j["success"] = success;
  j["stdout"] = stdout_content;
  j["stderr"] = stderr_content;
  j["error"] = error;
  j["exit_code"] = exit_code;
  return j.dump();
}

}  // namespace pu::tools
