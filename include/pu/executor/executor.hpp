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
#include "pu/agent_config.hpp"   // 修正为正确路径

namespace pu {

class Executor {
public:
  explicit Executor(Toolbox* toolbox);

  void SetSecurityPolicy(const config::SecurityPolicy& policy);

  std::string Execute(const std::string& input,
                      Workspace& workspace,
                      LLMProvider* provider);

private:
  struct ToolLoopResult {
    std::string final_response;
    bool completed = true;
    int tool_call_count = 0;
  };

  ToolLoopResult RunToolLoop(Workspace& workspace, LLMProvider* provider);

  Toolbox* toolbox_;
  std::optional<config::SecurityPolicy> security_policy_;
  int next_tool_call_id_ = 0;
};

} // namespace pu
