// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>
#include <optional>

#include "pu/llm/llm_provider.hpp"
#include "pu/tools/toolbox.hpp"
#include "pu/session/workspace.hpp"
#include "pu/session/call_stack.hpp"
#include "pu/conversation.hpp"
#include "pu/agent_config.hpp"

namespace pu {

class Executor {
public:
  explicit Executor(Toolbox* toolbox);

  void SetSecurityPolicy(const config::SecurityPolicy& policy);

  // Stage 6+7: allow the Runtime to swap the Toolbox when the active agent
  // changes (the Toolbox is rebuilt per agent).
  void SetToolbox(Toolbox* toolbox) { toolbox_ = toolbox; }

  // Returns error message if an error occurred, empty string otherwise.
  std::string Execute(const std::string& input,
                      Workspace& workspace,
                      LLMProvider* provider);

private:
  struct ToolLoopResult {
    std::string final_response;
    bool completed = true;
    int tool_call_count = 0;
    bool has_error = false;
    std::string error_message;
  };

  ToolLoopResult RunToolLoop(Workspace& workspace, LLMProvider* provider);

  Toolbox* toolbox_;
  std::optional<config::SecurityPolicy> security_policy_;
};

} // namespace pu
