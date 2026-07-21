// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>

#include "pu/context.hpp"
#include "pu/stack.hpp"
#include "pu/agent_core.hpp"

namespace pu {

class Orchestrator {
 public:
  Orchestrator(std::shared_ptr<GlobalContext> ctx,
               std::shared_ptr<CallStack> stack,
               agent::AgentManager& manager);

  bool HandleCommand(const std::string& input, std::string& output);
  std::string Process(const std::string& input);
  void Push(const std::string& agent_name);
  void Pop();
  std::string ShowStack() const;

 private:
  std::shared_ptr<GlobalContext> ctx_;
  std::shared_ptr<CallStack> stack_;
  agent::AgentManager& manager_;
};

}  // namespace pu
