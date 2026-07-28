// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include "pu/llm/llm_provider.hpp"
#include "pu/agent_config.hpp"

namespace pu {
class ForkMergeService;
class CallStack;
}

namespace pu {

struct ToolContext {
  const config::SecurityPolicy* security = nullptr;
  std::function<bool(const std::string& message)> request_confirmation;
  std::shared_ptr<ForkMergeService> fork_service;
  std::shared_ptr<CallStack> call_stack;  // needed by fork tools to push/pop delegations
};

class Tool {
 public:
  virtual ~Tool() = default;
  virtual std::string Name() const = 0;
  virtual std::string Description() const = 0;
  virtual std::string ParametersSchema() const = 0;
  virtual std::string Execute(const nlohmann::json& args, ToolContext& ctx) = 0;
};

}  // namespace pu