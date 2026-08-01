// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "pu/agent_config.hpp"
#include "pu/conversation.hpp"
#include "pu/core/fork_merge_service.hpp"
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
  void SetCompactionConfig(const config::HistoryCompactionConfig& cfg) { compaction_config_ = cfg; }
  void SetForkService(std::shared_ptr<ForkMergeService> fs) { fork_service_ = std::move(fs); }
  void SetCallStack(std::shared_ptr<CallStack> cs) { call_stack_ = std::move(cs); }

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
  config::HistoryCompactionConfig compaction_config_;
  std::shared_ptr<ForkMergeService> fork_service_;
  std::shared_ptr<CallStack> call_stack_;
};

}  // namespace pu
