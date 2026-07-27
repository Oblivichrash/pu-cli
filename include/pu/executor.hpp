// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>

#include "pu/agent_core.hpp"
#include "pu/core/context.hpp"
#include "pu/core/delegation_stack.hpp"

namespace pu::agent {

class AgentExecutor {
 public:
  explicit AgentExecutor(AgentManager& manager);

  void SetRootContext(std::shared_ptr<core::Context> root_context);
  void SetDelegationStack(std::shared_ptr<core::DelegationStack> stack);

  std::shared_ptr<core::DelegationStack> GetDelegationStack() const { return stack_; }

  AgentContext PrepareContext(const std::string& agent_name,
                              std::shared_ptr<core::Context> external_ctx = nullptr);
  std::string Execute(const std::string& agent_name, const std::string& input, AgentContext& ctx);
  std::string Dispatch(const std::string& input);

 private:
  std::string CallAgent(const std::string& agent_name, const std::string& input);
  AgentManager& manager_;
  std::shared_ptr<core::Context> root_context_;
  std::shared_ptr<core::DelegationStack> stack_;
};

}  // namespace pu::agent