// SPDX-License-Identifier: GPL-3.0-only
#include "pu/runtime/executor.hpp"

#include "pu/core/platform.hpp"
#include "pu/core/text.hpp"
#include "pu/core/logging.hpp"
#include "pu/llm/projection.hpp"
#include "pu/session/request.hpp"
#include "pu/tools/toolbox.hpp"

#include <boost/json.hpp>
#include "pu/core/json.hpp"
#include <spdlog/spdlog.h>

#include <chrono>
#include <algorithm>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <winternl.h>
#endif

namespace pu {

namespace {

std::string JoinQuoted(const std::vector<std::string>& names) {
  std::string joined;
  for (const std::string& name : names) {
    if (!joined.empty()) joined += ", ";
    joined += "\"" + name + "\"";
  }
  return joined;
}

std::string StopNotice(const std::string& finish_reason) {
  if (finish_reason.empty()) return "";
  if (finish_reason == "length") {
    return "The reply stopped at a token limit, so it may be incomplete. Set "
           "max_tokens for this backend to lengthen it, or leave it unset to use the "
           "provider's own limit.";
  }
  if (finish_reason == "content_filter") {
    return "The provider stopped the reply because its content filter matched.";
  }
  spdlog::debug("Provider ended the reply with finish_reason={}", finish_reason);
  return "";
}

#ifdef _WIN32
std::string WindowsKernelVersion() {
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
      ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  if (rtl_get_version) {
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtl_get_version(&info) == 0) {
      return std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) + "." +
             std::to_string(info.dwBuildNumber);
    }
  }
  return "unknown";
}
#endif

#ifndef _WIN32
std::string RunShellCapture(const std::string& cmd) {
  std::string output;
  pu::platform::ExecuteCommand(cmd, output);
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r' ||
                             output.back() == ' ' || output.back() == '\t')) {
    output.pop_back();
  }
  return output;
}
#endif

std::string OsName() {
#ifdef _WIN32
  return "Windows";
#else
  return RunShellCapture("uname -s");
#endif
}

std::string OsKernelVersion() {
#ifdef _WIN32
  return WindowsKernelVersion();
#else
  return RunShellCapture("uname -r");
#endif
}

}  // namespace

void Executor::ProbeStaticEnvironment() {
  if (static_env_info_.probed) return;

  static_env_info_.os_name = OsName();
  static_env_info_.kernel_version = OsKernelVersion();
  static_env_info_.probed = true;

  spdlog::debug("Probed environment: OS='{}' kernel='{}'", static_env_info_.os_name,
                static_env_info_.kernel_version);
}

std::string Executor::BuildStaticSystemContext() const {
  std::ostringstream oss;

  oss << "=== Environment ===\n";
  oss << "OS: " << static_env_info_.os_name << "\n";
  oss << "Kernel: " << static_env_info_.kernel_version << "\n";

  oss << "=== Security Policy ===\n";
  if (security_policy_) {
    oss << "Sandbox root: " << security_policy_->sandbox_root << "\n";
    oss << "Forbidden patterns: ";
    if (security_policy_->forbidden_patterns.empty()) {
      oss << "(none)";
    } else {
      for (size_t i = 0; i < security_policy_->forbidden_patterns.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << "'" << security_policy_->forbidden_patterns[i] << "'";
      }
    }
    oss << "\n";
  } else {
    oss << "(no security policy set)\n";
  }

  oss << "=== Working Directory ===\n";
  if (security_policy_ && !security_policy_->sandbox_root.empty()) {
    oss << security_policy_->sandbox_root << "\n";
  } else {
    oss << ".\n";
  }

  oss << "=== Tool Use Guidelines ===\n";
  oss << "1. Before calling any tool, output a concise step-by-step plan. Only execute tools after "
         "stating the plan.\n";
  oss << "2. When inspecting files, use targeted commands (head -n 50, tail -n 50, grep, sed -n "
         "'10,30p') instead of full cat dumps.\n";
  oss << "3. Use parallel tool calls when possible to minimize round trips.\n";

  return oss.str();
}

Executor::Executor() {
  approval_.ask = [this](const std::string& command, std::string& note) {
    return AskApproval(command, note);
  };
  ProbeStaticEnvironment();
}

void Executor::SetSecurityPolicy(const config::SecurityPolicy& policy) {
  security_policy_ = policy;
}

bool Executor::AskApproval(const std::string& command, std::string& note) {
  {
    std::lock_guard<std::mutex> lock(approval_mtx_);
    approval_answered_ = false;
    approval_answer_note_.clear();
  }
  if (on_approval_ask) on_approval_ask(command);

  std::unique_lock<std::mutex> lock(approval_mtx_);
  approval_cv_.wait(lock, [this] { return approval_answered_; });
  note = approval_answer_note_;
  return approval_allowed_;
}

ExecutionResult Executor::Execute(const std::string& input, Conversation& conversation,
                                  LLMProvider* provider, Toolbox& toolbox,
                                  CancelToken cancel_token,
                                  TokenCallback content_callback,
                                  ToolCallbacks tool_callbacks,
                                  TokenCallback reasoning_callback,
                                  CancelToken command_stop_token) {
  // The turn keeps who asked. It is the one place the origin is known before the user message
  // is written, and the page reads it back rather than inferring it from the text.
  ChatMessage user_turn;
  user_turn.role = session::kUserRole;
  user_turn.content = input;
  user_turn.from_peer = peer_origin_;
  conversation.Append(user_turn);


  const Turn turn{provider,          &toolbox,          std::move(content_callback),
                  std::move(tool_callbacks), std::move(reasoning_callback), cancel_token,
                  command_stop_token};

  ExecutionResult result = RunToolLoop(conversation, turn);
  if (result.has_error) return result;

  if (!result.content.empty()) conversation.Append("assistant", result.content);
  return result;
}

ToolContext Executor::BuildToolContext(const Turn& turn) {
  ToolContext tool_ctx;
  if (security_policy_.has_value()) {
    tool_ctx.security = &security_policy_.value();
  } else {
    static const config::SecurityPolicy empty_policy;
    tool_ctx.security = &empty_policy;
    spdlog::warn("No security policy set for Executor. Using empty policy.");
  }

  control_.stop_token = turn.command_stop_token;
  control_.note = user_note_;
  user_note_.clear();

  if (turn.command_stop_token) tool_ctx.control = &control_;

  tool_ctx.approval = approval_asks_ ? &approval_ : nullptr;

  // Read here and cleared: a turn from a peer reaches the tool set once, and a later turn in
  // the same session is the user's own.
  tool_ctx.peer_origin = peer_origin_;
  peer_origin_.clear();

  return tool_ctx;
}

void Executor::AppendAssistantTurn(Conversation& conversation, const ChatResult& chat_result) const {
  boost::json::array calls;
  for (const ToolCall& call : chat_result.tool_calls) {
    calls.push_back(session::ToolCallToJson(session::ToolCallRecord{call.id, call.name,
                                                                   call.arguments}));
  }

  ChatMessage assistant_msg;
  assistant_msg.role = session::kAssistantRole;
  assistant_msg.content = chat_result.content;
  assistant_msg.reasoning = chat_result.reasoning;
  assistant_msg.tool_calls = std::move(calls);
  conversation.Append(assistant_msg);
}

ChatResult Executor::CallProvider(Conversation& conversation, const Turn& turn,
                                  ExecutionResult& result, bool& failed, bool& stopped,
                                  bool& abandoned) {
  session::RequestInputs inputs;
  inputs.system_prompt = system_prompt_;
  inputs.environment = BuildStaticSystemContext();

  std::vector<ChatMessage> history = session::BuildRequestPath(conversation.Graph(), inputs);
  std::vector<ToolDefinition> tools = turn.toolbox->ToolDefinitions();

  ChatRequest request;
  request.history = history;
  request.tools = tools;
  request.cancel_token = turn.cancel_token;
  request.on_reasoning = turn.reasoning_callback;
  request.on_content = [&](std::string_view token) {
    if (token.empty()) return;
    result.was_streamed = true;
    result.content.append(token);
    if (turn.content_callback) {
      turn.content_callback(token);
    } else {
      std::cout << token << std::flush;
    }
  };

  try {
    ChatResult chat_result = turn.provider->Chat(request);
    if (chat_result.usage) {
      // One turn bills once per round the tool loop runs, so the turn's usage is the
      // sum across rounds, not the last round's. The counts are the provider's own
      // figures, summed as they were reported; `credit` adds the same way, since it
      // is a per-call cost.
      if (!result.usage) result.usage = TokenUsage{};
      result.usage->prompt_tokens += chat_result.usage->prompt_tokens;
      result.usage->completion_tokens += chat_result.usage->completion_tokens;
      result.usage->total_tokens += chat_result.usage->total_tokens;
      result.usage->cached_tokens += chat_result.usage->cached_tokens;
      // A cost is summed only where every round reported one, and only within one unit: a
      // total that mixed a priced round with an unpriced one would read as the turn's price
      // when it is part of it.
      if (chat_result.usage->cost) {
        if (!result.usage->cost) {
          result.usage->cost = chat_result.usage->cost;
        } else if (result.usage->cost->unit == chat_result.usage->cost->unit) {
          result.usage->cost->amount += chat_result.usage->cost->amount;
        } else {
          spdlog::warn("a turn priced in two units; keeping the first ('{}')",
                       result.usage->cost->unit);
        }
      } else if (result.usage->cost) {
        // One round in this turn said nothing about price. The sum would be short, so the
        // turn keeps no total rather than a number that undercounts it.
        result.usage->cost.reset();
      }
      spdlog::debug("tokens this round: prompt={}, completion={}, total={}, cached={}, cost={}",
                    chat_result.usage->prompt_tokens, chat_result.usage->completion_tokens,
                    chat_result.usage->total_tokens, chat_result.usage->cached_tokens,
                    chat_result.usage->cost ? std::to_string(chat_result.usage->cost->amount) +
                                                  " " + chat_result.usage->cost->unit
                                            : std::string("reported none"));
    }
    if (!chat_result.raw_usage.is_null()) {
      spdlog::debug("usage as the provider reported it: {}", json::serialize(chat_result.raw_usage));
    }
    if (!chat_result.unread_fields.empty()) {
      // Logged rather than noted: the standard stream metadata is unread on every turn, and
      // a note that always appears is a note nobody reads.
      spdlog::warn("fields on the reply nothing reads: {}",
                   boost::json::serialize(boost::json::array(chat_result.unread_fields.begin(),
                                                             chat_result.unread_fields.end())));
    }
    if (!chat_result.missing_fields.empty()) {
      spdlog::warn("fields this build reads that the reply never sent: {}",
                   boost::json::serialize(boost::json::array(chat_result.missing_fields.begin(),
                                                             chat_result.missing_fields.end())));
      result.shape_notes.emplace_back("the reply never carried " +
                               JoinQuoted(chat_result.missing_fields) +
                               "; this build reads a name the gateway no longer sends");
    }
    return chat_result;
  } catch (const std::exception& e) {
    const bool cancelled =
        turn.cancel_token && turn.cancel_token->load(std::memory_order_acquire);
    if (cancelled) {
      spdlog::debug("Request stopped by the caller: {}", e.what());
      stopped = true;
      return {};
    }
    if (platform::ShutdownRequested()) {
      spdlog::debug("Request abandoned as the process shut down: {}", e.what());
      abandoned = true;
      return {};
    }
    failed = true;
    result.has_error = true;
    result.error_message = "Request failed: " + text::ReadableError(e.what());
    return {};
  }
}

void Executor::RunToolCalls(Conversation& conversation, const Turn& turn,
                            const std::vector<ToolCall>& calls, ExecutionResult& result) {
  ToolContext tool_ctx = BuildToolContext(turn);

  for (const ToolCall& call : calls) {
    if (turn.cancel_token && turn.cancel_token->load(std::memory_order_acquire)) {
      spdlog::debug("Turn cancelled before tool '{}' ran", call.name);
      break;
    }

    ++result.tool_call_count;

    if (turn.tool_callbacks.on_start) {
      turn.tool_callbacks.on_start(call.id, call.name, call.arguments);
    }

    std::string tool_result;
    SetLogToolName(call.name);
    const auto tool_start = std::chrono::steady_clock::now();
    try {
      tool_result = turn.toolbox->ExecuteTool(call.name, call.arguments, tool_ctx);
    } catch (const std::exception& e) {
      tool_result = tools::MakeToolResultJson(false, "",
                                              std::string("Tool execution error: ") + e.what(), -1);
    }
    const auto tool_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              tool_start)
            .count();
    SetLogDurationMs(tool_ms);
    spdlog::info("Tool '{}' completed in {} ms", call.name, tool_ms);
    ClearLogToolName();
    ClearLogDurationMs();

    const auto parsed = tools::ParseToolResult(tool_result);
    if (parsed.valid && parsed.blocked) {
      ++result.blocked_tool_calls;
      if (parsed.refusal == tools::Refusal::kNobodyToAsk) ++result.unaskable_tool_calls;
    }

    if (turn.tool_callbacks.on_end) {
      if (parsed.valid) {
        turn.tool_callbacks.on_end(call.id, call.name, call.arguments, parsed.output, parsed.error);
      } else {
        turn.tool_callbacks.on_end(call.id, call.name, call.arguments, tool_result, "");
      }
    }

    ChatMessage tool_msg;
    tool_msg.role = "tool";
    tool_msg.content = tool_result;
    tool_msg.tool_name = call.name;
    tool_msg.tool_call_id = call.id;
    conversation.Append(tool_msg);
  }

  // The note rides after every receipt of this round: inside the loop it would land
  // between two tool receipts, and a user message splitting the assistant's
  // tool_calls from their receipts is answered 400 by the gateway on the next round.
  if (!user_note_.empty() && turn.cancel_token) {
    ChatMessage note_msg;
    note_msg.role = "user";
    note_msg.content = "[user note during the turn] " + user_note_;
    conversation.Append(note_msg);
    user_note_.clear();
  }

  if (turn.cancel_token && turn.cancel_token->load(std::memory_order_acquire)) {
    conversation.SealInterruptedToolCalls();
  }
}

ExecutionResult Executor::RunToolLoop(Conversation& conversation, const Turn& turn) {
  ExecutionResult result;
  bool budget_reached = false;
  bool wrap_up_asked = false;
  bool abandoned = false;

  for (;;) {
    // The budget counts tool calls, not replies: one reply can carry several calls. The check
    // sits at the top of a round, so a round whose reply *reaches* the budget has already run —
    // a budget of 2 lets two calls happen and asks for the wrap-up after them, rather than
    // stopping the round that made the second one.
    if (!budget_reached && result.tool_call_count >= tool_call_budget_) {
      budget_reached = true;
      spdlog::warn("Tool-call budget of {} reached, asking the model to wrap up",
                   tool_call_budget_);
    }

    if (budget_reached && !wrap_up_asked) {
      // Rather than ending the turn with an error, spend one round on a wrap-up: the
      // model answers from what it has gathered, and the user reads an answer instead
      // of a refusal. A wrap-up that still calls a tool is answered no further.
      ChatMessage wrap_up;
      wrap_up.role = session::kUserRole;
      wrap_up.content =
          "[system] The tool-call budget for this turn is exhausted. Do not call any "
          "more tools; answer now from what you have gathered so far.";
      conversation.Append(wrap_up);
      wrap_up_asked = true;
    } else if (budget_reached && wrap_up_asked) {
      spdlog::warn("Wrap-up round made no final answer, ending the turn");
      break;
    }

    bool failed = false;
    bool stopped = false;
    ++result.rounds;
    ChatResult chat_result = CallProvider(conversation, turn, result, failed, stopped, abandoned);
    if (failed) break;
    if (stopped) {
      conversation.SealInterruptedToolCalls();
      return result;
    }
    if (abandoned) return result;

    if (chat_result.tool_calls.empty()) {
      std::string response = chat_result.content;
      const std::string reasoning = llm::ProjectReasoningText(chat_result.reasoning);
      if (response.empty() && !reasoning.empty()) {
        response = reasoning;
        spdlog::debug("Using reasoning as final response (thinking mode)");
      }
      result.content = response;
      result.notice = StopNotice(chat_result.finish_reason);
      if (!chat_result.model.empty()) {
        result.model = chat_result.model;
        spdlog::debug("Answered by {}", chat_result.model);
      }
      break;
    }

    for (ToolCall& call : chat_result.tool_calls) {
      if (call.id.empty()) {
        call.id = "call_" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                  "_" + std::to_string(++next_tool_call_id_);
      }
    }

    AppendAssistantTurn(conversation, chat_result);
    RunToolCalls(conversation, turn, chat_result.tool_calls, result);
  }

  if (budget_reached && result.content.empty()) {
    result.outcome = TurnOutcome::kToolBudgetReached;
    result.content =
        "The tool-call budget for this turn was reached without a final answer. "
        "Please rephrase your request or narrow the scope, or raise the budget with "
        "the tool_call_budget agent setting.";
    result.has_error = true;
    return result;
  }

  if (result.content.empty() && result.blocked_tool_calls > 0 &&
      result.blocked_tool_calls == result.tool_call_count) {
    // Nothing ran and nothing was said: the sandbox is why, and telling the caller to retry
    // would be wrong. The refusals themselves are in the conversation for the model.
    result.only_blocked_calls = true;
    result.only_unaskable_calls = result.unaskable_tool_calls == result.blocked_tool_calls;
    if (!result.has_error) {
      result.has_error = true;
      result.error_message =
          result.only_unaskable_calls
              ? "Every tool call this turn made needed approval, and nothing here can ask: "
                "give the agent a command that needs none, or take it off approval_patterns."
              : "Every tool call this turn made was refused by the sandbox; nothing ran.";
    }
    return result;
  }

  if (!result.has_error && result.content.empty() && result.tool_call_count == 0) {
    result.has_error = true;
    result.error_message =
        "Model returned an empty response without any tool calls. "
        "Please check the backend service or try again.";
    return result;
  }

  if (result.content.empty() && result.tool_call_count > 0) {
    spdlog::info("Tool execution completed without a final text response - considered successful.");
  }

  for (const std::string& note : result.shape_notes) {
    if (!result.notice.empty()) result.notice += "\n";
    result.notice += note;
  }

  return result;
}

}  // namespace pu
