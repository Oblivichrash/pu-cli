// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include "pu/llm/llm_provider.hpp"

// Forward declarations
namespace pu {
class ForkMergeService;
}

namespace pu {
namespace agent {
namespace config {
struct SecurityPolicy;
}
}
}

namespace pu {

struct ToolContext {
  const agent::config::SecurityPolicy* security = nullptr;
  std::function<bool(const std::string& message)> request_confirmation;
  std::shared_ptr<pu::ForkMergeService> fork_service;
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
  void RemoveTool(const std::string& name);
  Tool* GetTool(const std::string& name) const;
  std::vector<ToolDefinition> GetToolDefinitions() const;
  std::string ExecuteTool(const std::string& name,
                          const nlohmann::json& args,
                          ToolContext& ctx);
  void ReloadExternalTools(const std::string& directory);

 private:
  std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
  std::unordered_map<std::string, std::string> tool_file_mtimes_;
};

}  // namespace pu
