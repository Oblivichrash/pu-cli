// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <vector>
#include <optional>

#include "pu/llm/llm_provider.hpp"
#include "pu/tools/toolbox.hpp"
#include "pu/session/workspace.hpp"
#include "pu/session/call_stack.hpp"
#include "pu/conversation.hpp"
#include "pu/agent_core.hpp"   // for agent::config::SecurityPolicy

namespace pu {

class Executor {
public:
  explicit Executor(Toolbox* toolbox);

  // Set the security policy to be used for all tool executions.
  void SetSecurityPolicy(const agent::config::SecurityPolicy& policy);

  std::string Execute(const std::string& input,
                      Workspace& workspace,
                      LLMProvider* provider);

private:
  struct ToolLoopResult {
    std::string final_response;
  };

  ToolLoopResult RunToolLoop(Workspace& workspace, LLMProvider* provider);

  Toolbox* toolbox_;
  std::optional<agent::config::SecurityPolicy> security_policy_;
};

} // namespace pu
