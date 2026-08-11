// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "pu/agent_config.hpp"
#include "pu/llm/llm_provider.hpp"
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

struct StaticEnvInfo {
  std::string os_name;
  std::string kernel_version;
  bool probed = false;
};

class Executor {
 public:
  explicit Executor(Toolbox* toolbox);

  void SetSecurityPolicy(const config::SecurityPolicy& policy);
  void SetToolbox(Toolbox* toolbox) { toolbox_ = toolbox; }
  void SetCompactionConfig(const config::HistoryCompactionConfig& cfg) { compaction_config_ = cfg; }

  ExecutionResult Execute(const std::string& input, Workspace& workspace,
                          LLMProvider* provider);

  const StaticEnvInfo& GetStaticEnvInfo() const { return static_env_info_; }
  std::string BuildStaticSystemContext() const;

 private:
  struct ToolLoopResult {
    std::string final_response;
    int tool_call_count = 0;
    bool has_error = false;
    bool was_streamed = false;
    std::string error_message;
  };

  ToolLoopResult RunToolLoop(Workspace& workspace, LLMProvider* provider);

  void ProbeStaticEnvironment();

  Toolbox* toolbox_;
  std::optional<config::SecurityPolicy> security_policy_;
  config::HistoryCompactionConfig compaction_config_;
  int next_tool_call_id_ = 0;

  StaticEnvInfo static_env_info_;
};

}  // namespace pu
