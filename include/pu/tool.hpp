// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/backend.hpp"
#include "pu/agent_config.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <vector>

namespace pu::agent {

struct ToolContext {
  std::string sandbox_root;
  std::vector<std::string> allowed_paths;
  size_t max_command_length = 0;
  std::vector<std::string> forbidden_patterns;
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

}  // namespace pu::agent
