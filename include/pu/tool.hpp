// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/backend.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <string>

namespace pu::agent {

struct ToolContext {
  std::string sandbox_root;
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
