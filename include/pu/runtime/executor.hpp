// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "pu/config/agents.hpp"
#include "pu/core/base.hpp"
#include "pu/llm/llm_provider.hpp"
#include "pu/session/request.hpp"
#include "pu/session/session.hpp"
#include "pu/agent_tools/toolbox.hpp"

namespace pu {

struct ToolCallbacks {
  std::function<void(const std::string& id, const std::string& name,
                     const boost::json::value& args)>
      on_start;
  // The name and arguments ride with the result as well as with the call: a page that
  // reconnected mid-command receives the end without the start, and what it draws has to
  // come from the end itself.
  //
  // `page` is the paging envelope the result carried, or null for a result that was not a
  // page. It is here rather than folded into `output` because the two answer different
  // questions: `output` is what the command printed, `page` is how much of that survived and
  // where the rest is. Without it a page of a 3500-line log draws exactly like a whole reply.
  std::function<void(const std::string& id, const std::string& name,
                     const boost::json::value& args, const std::string& output,
                     const std::string& error, const boost::json::value& page)>
      on_end;
};

// How a turn ended.
enum class TurnOutcome {
  kCompleted,
  kFailed,
};

// What one provider call did. The four states are exclusive, which is why they are one value
// rather than three flags a caller would have to keep mutually exclusive by hand.
enum class ProviderOutcome {
  kReplied,
  kFailed,
  kStopped,
  kAbandoned,
};

struct ProviderCall {
  ChatResult reply;
  ProviderOutcome outcome = ProviderOutcome::kReplied;
};

struct ExecutionResult {
  std::string content;
  bool was_streamed = false;
  int tool_call_count = 0;
  std::optional<TokenUsage> usage;
  // How many provider calls this turn made. One reply that calls three tools is two rounds,
  // and the token counts are both their sum; without this the sum has no explanation.
  int rounds = 0;
  std::string notice;
  std::string model;
  TurnOutcome outcome = TurnOutcome::kCompleted;
  bool has_error = false;
  std::string error_message;
  // Tools this turn asked for and the sandbox refused: a forbidden pattern, a dangerous
  // command, or an approval nobody could give. Counted rather than logged because the
  // sandbox is the one refusal a caller may not retry, and only this count tells it apart
  // from a provider that failed.
  int blocked_tool_calls = 0;
  // Of those, the ones refused because the front end had no way to ask for approval. That
  // is a configuration to fix, not a command to rework, so the caller's remedy differs.
  int unaskable_tool_calls = 0;
  // Every tool call this turn made was refused by the sandbox, and no answer came back.
  // A turn that recovered by running something else is not this: its refusal is advice the
  // model already acted on, not the turn's outcome.
  bool only_blocked_calls = false;
  // Those refusals were all of the unaskable kind: nothing is wrong with the command.
  bool only_unaskable_calls = false;
  // The line was a command the router answered, so no provider was called and `content` is the
  // command's own output. A fact about this turn like `has_error` is, and carried here for the
  // same reason: the caller that prints the answer and the caller that picks an exit status
  // both need it, and neither should have to be handed a second out-parameter to get it.
  bool was_command = false;
};

class Executor {
 public:

  Executor();

  void SetSecurityPolicy(const config::SecurityPolicy& policy);

  void SetSystemPrompt(std::string prompt) { system_prompt_ = std::move(prompt); }

  // How much context the model this turn asks holds, or 0 when the entry names none. Advisory:
  // nothing is trimmed against it and no turn is refused for outgrowing it. It is read for two
  // things a caller cannot see without it -- how full the last request was, which the near-window
  // notice states, and what "exceeds the available context size" from a gateway actually means.
  void SetContextWindow(int tokens) { context_window_ = tokens; }

  // Who is answering this turn, in the runtime's own words. Carried into the static context
  // because a prompt a person wrote says what an agent is for, and an agent that has been
  // switched to mid-session otherwise answers as whichever agent the model last heard about.
  // Set beside the system prompt, which is the same part of the same entry.
  void SetAgentIdentity(std::string name, std::string description) {
    agent_name_ = std::move(name);
    agent_description_ = std::move(description);
  }

  void SetUserNote(std::string note) { user_note_ = std::move(note); }

  // How the executor learns it should stop, beyond the token its caller passed. A token covers a
  // caller that can press a button; the runtime also holds a deadline and knows whether the
  // process is shutting down, and those two have no token to hand over. Asked at the top of every
  // round and before every tool call, so a deadline lands between them rather than after the
  // whole turn.
  void SetStopCheck(std::function<bool()> check) { stop_check_ = std::move(check); }

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

  // Everything one turn needs beyond its input and the conversation it writes to. Named
  // fields because half of these are callbacks and two of them share a type: a positional
  // call site could not tell the two `TokenCallback`s apart. Every field starts empty, so a
  // call site names only what it carries.
  struct Turn {
    LLMProvider* provider = nullptr;
    Toolbox* toolbox = nullptr;
    TokenCallback content_callback = nullptr;
    ToolCallbacks tool_callbacks = {};
    TokenCallback reasoning_callback = nullptr;
    CancelToken cancel_token = nullptr;
    // A second token, because a command kills its own turn without cancelling the turn that
    // asked for it: `ask_dir` passes the asking turn's token here so a stop reaches the child.
    CancelToken command_stop_token = nullptr;
  };

  // `attachments` ride on the message this turn appends rather than following it: a prompt and
  // the picture it asks about are one turn, and a second message would be a second question.
  ExecutionResult Execute(const std::string& input, Conversation& conversation, Turn turn,
                          std::vector<ContentBlock> attachments = {});

  std::string BuildStaticSystemContext() const;
  // Folds what tools spent into the turn's own account, so a receipt covers the whole turn.
  void AddToolSpending(ExecutionResult& result) const;

 private:
  ExecutionResult RunToolLoop(Conversation& conversation, const Turn& turn);

  ProviderCall CallProvider(Conversation& conversation, const Turn& turn, ExecutionResult& result);

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
  std::string agent_name_;
  std::string agent_description_;
  // What tools spent on the turn's behalf. Cleared at the start of a turn, and added to the
  // provider's own count before the result is returned.
  ToolContext::Spending spent_by_tools_;
  int next_tool_call_id_ = 0;

  // From the agent's own `context_window`; 0 when it names none. See SetContextWindow.
  int context_window_ = 0;

  std::function<bool()> stop_check_;

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
