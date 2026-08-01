// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "pu/agent_config.hpp"
#include "pu/conversation.hpp"
#include "pu/llm/llm_provider.hpp"
#include "pu/session/call_stack.hpp"
#include "pu/session/workspace.hpp"
#include "pu/tools/toolbox.hpp"

namespace pu {

struct ExecutionResult {
  std::string content;
  bool was_streamed = false;
  bool has_error = false;
  std::string error_message;
  int tool_call_count = 0;
};

class Executor {
 public:
  explicit Executor(Toolbox* toolbox);

  void SetSecurityPolicy(const config::SecurityPolicy& policy);
  void SetToolbox(Toolbox* toolbox) { toolbox_ = toolbox; }

  ExecutionResult Execute(const std::string& input, Workspace& workspace,
                          LLMProvider* provider);

 private:
  struct ToolLoopResult {
    std::string final_response;
    bool completed = true;
    int tool_call_count = 0;
    bool has_error = false;
    bool was_streamed = false;
    std::string error_message;
  };

  ToolLoopResult RunToolLoop(Workspace& workspace, LLMProvider* provider);

  Toolbox* toolbox_;
  std::optional<config::SecurityPolicy> security_policy_;
};

}  // namespace pu
