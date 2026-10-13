// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "pu/config/agents.hpp"
#include "pu/core/base.hpp"
#include "pu/llm/llm_provider.hpp"
#include "pu/session/request.hpp"
#include "pu/session/session.hpp"
#include "pu/tools/toolbox.hpp"

namespace pu {

struct ToolCallbacks {
  std::function<void(const std::string& id, const std::string& name,
                     const boost::json::value& args)>
      on_start;
  std::function<void(const std::string& id, const std::string& output, const std::string& error)>
      on_end;
};

struct ExecutionResult {
  std::string content;
  bool was_streamed = false;
  int tool_call_count = 0;
  std::optional<TokenUsage> usage;
  std::string notice;
  std::string model;
  bool has_error = false;
  std::string error_message;
};

class Executor {
 public:
  Executor();

  void SetSecurityPolicy(const config::SecurityPolicy& policy);

  void SetSystemPrompt(std::string prompt) { system_prompt_ = std::move(prompt); }

  // The user's words typed while a turn runs. The tool loop drains it: into the
  // stop result when a command is killed, beside a tool result otherwise.
  void SetUserNote(std::string note) { user_note_ = std::move(note); }

  // A command matching the policy's approval patterns parks in AskApproval until
  // ResolveApproval answers it.
  void EnableApprovalGate() { approval_asks_ = true; }
  void SetOnApprovalAsk(std::function<void(const std::string& command)> cb) {
    on_approval_ask = std::move(cb);
  }
  // Answers the pending ask; without one, a harmless no-op the waiting thread
  // alone reads.
  void ResolveApproval(bool allow, std::string note) {
    {
      std::lock_guard<std::mutex> lock(approval_mtx_);
      approval_allowed_ = allow;
      approval_answer_note_ = std::move(note);
      approval_answered_ = true;
    }
    approval_cv_.notify_all();
  }

  ExecutionResult Execute(const std::string& input, Conversation& conversation,
                          LLMProvider* provider, Toolbox& toolbox,
                          CancelToken cancel_token = nullptr,
                          TokenCallback content_callback = nullptr,
                          ToolCallbacks tool_callbacks = {},
                          TokenCallback reasoning_callback = nullptr,
                          CancelToken command_stop_token = nullptr);

  std::string BuildStaticSystemContext() const;

 private:
  ExecutionResult RunToolLoop(Conversation& conversation, LLMProvider* provider, Toolbox& toolbox,
                              CancelToken cancel_token,
                              TokenCallback content_callback,
                              ToolCallbacks tool_callbacks,
                              TokenCallback reasoning_callback,
                              CancelToken command_stop_token = nullptr);

  void ProbeStaticEnvironment();

  // Parks the tool thread until ResolveApproval answers; the answer's note rides
  // back beside the command.
  bool AskApproval(const std::string& command, std::string& note);

  std::string user_note_;

  // One pending ask, answered from the serve thread.
  bool approval_asks_ = false;
  std::function<void(const std::string&)> on_approval_ask;
  std::mutex approval_mtx_;
  std::condition_variable approval_cv_;
  bool approval_answered_ = false;
  bool approval_allowed_ = false;
  std::string approval_answer_note_;
  std::string approval_pending_command_;
  std::string pending_approval_note_;

  std::optional<config::SecurityPolicy> security_policy_;
  std::string system_prompt_;
  int next_tool_call_id_ = 0;

  struct StaticEnvInfo {
    std::string os_name;
    std::string kernel_version;
    bool probed = false;
  };
  StaticEnvInfo static_env_info_;
};

}  // namespace pu
