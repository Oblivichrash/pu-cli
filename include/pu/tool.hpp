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

}  // namespace pu::agent
