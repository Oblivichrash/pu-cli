// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "pu/agent_config.hpp"
#include "pu/core/cancel_token.hpp"
#include "pu/llm/llm_provider.hpp"
#include "pu/session/request.hpp"
#include "pu/session/workspace.hpp"
#include "pu/tools/toolbox.hpp"

namespace pu {

// Tool call lifecycle callbacks for streaming/UI feedback.
struct ToolCallbacks {
  // Called before a tool is executed.
  std::function<void(const std::string& id,
                     const std::string& name,
                     const boost::json::value& args)> on_start;
  // Called after a tool completes execution.
  std::function<void(const std::string& id,
                     const std::string& output,
                     const std::string& error)> on_end;
};

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

  // The agent's configured prompt, supplied by the runtime rather than read back
  // out of session state, so a request depends only on the conversation and the
  // named inputs the caller provides.
  void SetSystemPrompt(std::string prompt) { system_prompt_ = std::move(prompt); }

  ExecutionResult Execute(const std::string& input, Workspace& workspace,
                          LLMProvider* provider,
                          CancelToken cancel_token = nullptr,
                          std::function<void(const std::string&)> content_callback = nullptr,
                          ToolCallbacks tool_callbacks = {});

  const StaticEnvInfo& GetStaticEnvInfo() const { return static_env_info_; }
  std::string BuildStaticSystemContext() const;

 private:
  struct ToolLoopResult {
    std::string final_response;
    bool completed = true;
    int tool_call_count = 0;
    bool has_error = false;
    bool was_streamed = false;
    std::string error_message;
  };

  ToolLoopResult RunToolLoop(Workspace& workspace, LLMProvider* provider,
                             CancelToken cancel_token,
                             std::function<void(const std::string&)> content_callback,
                             ToolCallbacks tool_callbacks);

  void ProbeStaticEnvironment();

  Toolbox* toolbox_;
  std::optional<config::SecurityPolicy> security_policy_;
  std::string system_prompt_;
  int next_tool_call_id_ = 0;

  StaticEnvInfo static_env_info_;
};

}  // namespace pu
