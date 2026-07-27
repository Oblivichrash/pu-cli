// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>

#include "pu/agent_core.hpp"
#include "pu/session/workspace.hpp"
#include "pu/session/call_stack.hpp"

namespace pu::agent {

class AgentExecutor {
 public:
  explicit AgentExecutor(AgentManager& manager);

  void SetRootContext(std::shared_ptr<Workspace> root_context);
  void SetDelegationStack(std::shared_ptr<CallStack> stack);

  std::shared_ptr<CallStack> GetDelegationStack() const { return stack_; }

  AgentContext PrepareContext(const std::string& agent_name,
                              std::shared_ptr<Workspace> external_ctx = nullptr);
  std::string Execute(const std::string& agent_name, const std::string& input, AgentContext& ctx);
  std::string Dispatch(const std::string& input);

 private:
  std::string CallAgent(const std::string& agent_name, const std::string& input);
  AgentManager& manager_;
  std::shared_ptr<Workspace> root_context_;
  std::shared_ptr<CallStack> stack_;
};

}  // namespace pu::agent
