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
  // The name and arguments ride with the result as well as with the call: a page that
  // reconnected mid-command receives the end without the start, and what it draws has to
  // come from the end itself.
  std::function<void(const std::string& id, const std::string& name,
                     const boost::json::value& args, const std::string& output,
                     const std::string& error)>
      on_end;
};

// How a turn ended. `kToolBudgetReached` means the wrap-up round also failed to
// produce an answer; a turn that was stopped at the budget but did answer is
// `kCompleted`, with the answer the wrap-up round produced.
enum class TurnOutcome {
  kCompleted,
  kFailed,
  kToolBudgetReached,
};

struct ExecutionResult {
  std::string content;
  bool was_streamed = false;
  int tool_call_count = 0;
  std::optional<TokenUsage> usage;
  std::string notice;
  // A shape change worth telling the user about, attached to `notice` rather than logged.
  std::vector<std::string> shape_notes;
  std::string model;
  TurnOutcome outcome = TurnOutcome::kCompleted;
  bool has_error = false;
  std::string error_message;
};

class Executor {
 public:
  Executor();

  void SetSecurityPolicy(const config::SecurityPolicy& policy);

  void SetSystemPrompt(std::string prompt) { system_prompt_ = std::move(prompt); }

  void SetToolCallBudget(int budget) { tool_call_budget_ = budget; }

  void SetUserNote(std::string note) { user_note_ = std::move(note); }

  void EnableApprovalGate() { approval_asks_ = true; }
  void SetOnApprovalAsk(std::function<void(const std::string& command)> cb) {
    on_approval_ask = std::move(cb);
  }
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
  struct Turn {
    LLMProvider* provider;
    Toolbox* toolbox;
    TokenCallback content_callback;
    ToolCallbacks tool_callbacks;
    TokenCallback reasoning_callback;
    CancelToken cancel_token;
    CancelToken command_stop_token;
  };

  ExecutionResult RunToolLoop(Conversation& conversation, const Turn& turn);

  ChatResult CallProvider(Conversation& conversation, const Turn& turn, ExecutionResult& result,
                          bool& failed, bool& stopped, bool& abandoned);

  void RunToolCalls(Conversation& conversation, const Turn& turn,
                    const std::vector<ToolCall>& calls, ExecutionResult& result);

  void AppendAssistantTurn(Conversation& conversation, const ChatResult& chat_result) const;

  ToolContext BuildToolContext(const Turn& turn);

  void ProbeStaticEnvironment();

  bool AskApproval(const std::string& command, std::string& note);

  std::string user_note_;

  bool approval_asks_ = false;
  std::function<void(const std::string&)> on_approval_ask;
  std::mutex approval_mtx_;
  std::condition_variable approval_cv_;
  bool approval_answered_ = false;
  bool approval_allowed_ = false;
  std::string approval_answer_note_;

  std::optional<config::SecurityPolicy> security_policy_;
  std::string system_prompt_;
  int next_tool_call_id_ = 0;

  // The tool-call budget for one turn: how many tool calls the model may make
  // before it is asked to wrap up. Counted per call, not per reply, since one
  // reply can carry several.
  inline static constexpr int kDefaultToolCallBudget = 100;

  int tool_call_budget_ = kDefaultToolCallBudget;

  ToolContext::Control control_;
  ToolContext::Approval approval_;

  struct StaticEnvInfo {
    std::string os_name;
    std::string kernel_version;
    bool probed = false;
  };
  StaticEnvInfo static_env_info_;
};

}  // namespace pu
