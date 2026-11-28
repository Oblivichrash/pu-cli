// SPDX-License-Identifier: GPL-3.0-only
#include "pu/runtime/executor.hpp"

#include "pu/core/platform.hpp"
#include "pu/core/text.hpp"
#include "pu/core/logging.hpp"
#include "pu/core/process.hpp"
#include "pu/llm/projection.hpp"
#include "pu/session/request.hpp"
#include "pu/agent_tools/toolbox.hpp"

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

// Said once per turn when the last request filled most of the window the agent's own
// `context_window` names. It is a statement, not a trim: the request went out whole, and a turn
// this one made the request for is a turn the caller has to end. Nothing here shortens anything.
//
// The share is approximate by construction. `prompt_tokens` is the provider's own count of one
// request, and the harness overhead of the next one -- tool definitions, the system block -- is
// not in it, so the boundary unit is a thought about the next request rather than a measurement
// of it. Saying so is the point: a ceiling nobody can see is what produces the gateway's own
// refusal a round later.
std::string WindowNotice(int prompt_tokens, int window) {
  if (window <= 0 || prompt_tokens <= 0) return "";
  const double share = static_cast<double>(prompt_tokens) / static_cast<double>(window);
  if (share < 0.8) return "";
  std::ostringstream oss;
  oss << "This turn's last request was " << prompt_tokens << " of the " << window
      << " tokens this model holds, so the conversation is nearly full. Nothing was shortened: "
         "end the turn, or hand the rest of the work to another agent with ask_dir.";
  return oss.str();
}

// What to say when a gateway refuses a request for being larger than the window it holds. The
// gateway's own sentence is kept under a diagnostic, because it names the figures; what is put
// in front of it is the part the reader has to act on, which the gateway has no way to state.
std::string TooLargeMessage(const std::string& said, int window) {
  std::ostringstream oss;
  oss << "The conversation outgrew this model's context window";
  if (window > 0) oss << " (" << window << " tokens, from this agent's context_window)";
  oss << ". Nothing was dropped to make it fit: start a new conversation, or hand the rest of "
         "the work to another agent with ask_dir.";
  if (!said.empty()) oss << " The provider said: " << said;
  return oss.str();
}

// Whether a `what()` names a request the provider refused for its size. Gateways spell this
// differently -- llama.cpp says "exceeds the available context size", OpenAI "maximum context
// length", Anthropic "prompt is too long" -- so the test is on the subject rather than on one
// product's wording, which is the mistake this function exists to stop being made twice.
bool RefusedForSize(std::string_view what) {
  const auto has = [what](std::string_view needle) {
    return what.find(needle) != std::string_view::npos;
  };
  return (has("context size") || has("context_length") || has("context length") ||
          has("maximum context") || has("prompt is too long") || has("too many tokens"));
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
  std::string output = pu::core::Run(cmd, pu::CancelToken{}, "", 0).captured.text;
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
  // Sent on every request of every turn, so every word here is paid for again each time. It is
  // written as the facts a model cannot read off the tool it is about to call, and no longer:
  // the headings earn their four words by making the block scannable, not by being pretty.
  std::ostringstream oss;

  // First, because every line below is read as this agent's view of the machine. Left out when
  // the entry named nothing: a name heading with nothing under it is a line the model spends
  // reasoning about rather than a fact.
  if (!agent_name_.empty() || !agent_description_.empty()) {
    oss << "=== Identity ===\n";
    if (!agent_name_.empty()) oss << "Agent: " << agent_name_ << "\n";
    if (!agent_description_.empty()) oss << "Role: " << agent_description_ << "\n";
  }

  // The working directory is the first thing a command resolves against, so it rides with the
  // environment rather than under a heading of its own.
  oss << "=== Environment ===\n";
  oss << "OS: " << static_env_info_.os_name << "\n";
  oss << "Kernel: " << static_env_info_.kernel_version << "\n";
  oss << "Working directory: ";
  if (security_policy_ && !security_policy_->sandbox_root.empty()) {
    oss << security_policy_->sandbox_root << "\n";
  } else {
    oss << ".\n";
  }

  oss << "=== Security Policy ===\n";
  if (security_policy_) {
    oss << "Sandbox root: " << security_policy_->sandbox_root << "\n";
    oss << "Forbidden patterns: ";
    if (security_policy_->forbidden_patterns.empty()) {
      oss << "(none)";
    } else {
      for (std::size_t i = 0; i < security_policy_->forbidden_patterns.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << "'" << security_policy_->forbidden_patterns[i] << "'";
      }
    }
    oss << "\n";
  } else {
    oss << "(no security policy set)\n";
  }

  // One line, because the distinction it draws is one distinction: a command's own exit code is
  // the command's, and these four are the program's.
  oss << "=== Exit Codes ===\n";
  oss << "0 ok, 1 bad invocation or configuration, 2 provider refused or unreachable, 3 sandbox "
         "refused every call, 4 stopped by a person. A `bash` result carries the command's own "
         "exit code, which is whatever the command meant by it.\n";

  oss << "=== Tool Use Guidelines ===\n";
  oss << "1. Plan in your reasoning; put a plan in the reply only when it tells the user "
         "something the work itself would not.\n";
  oss << "2. Read the end you need, not the whole file: a file you are about to edit from its "
         "head, a log or a failure from its tail with at='tail'.\n";
  oss << "3. Output comes back one page at a time. Pass page.later or page.earlier back as `at` "
         "to read the next one instead of running the command again; a page is stored, and "
         "`grep` is for what you have not stored.\n";
  oss << "4. Make parallel tool calls when they do not depend on each other.\n";
  // A delegated answer is already prose somebody finished writing. The one continuation this
  // has to stop is the cheapest a model can make: reading a paragraph and then writing the
  // same paragraph again. Stated here rather than in ask_dir's own description, because a
  // description says what a tool is, while this says what to do with what came back.
  oss << "5. A conclusion a tool returns -- a delegated answer, a review, a summary -- is passed "
         "on, not restated: say what you asked, what you did with it, and what is still open.\n";

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
                                  Turn turn, std::vector<ContentBlock> attachments) {
  ChatMessage user_turn;
  user_turn.role = kUserRole;
  user_turn.SetText(input);
  // The attachments ride on this message rather than following it: a prompt and the picture it
  // asks about are one turn, and a second message would be a second question.
  for (ContentBlock& block : attachments) user_turn.content.push_back(std::move(block));
  conversation.Append(user_turn);

  // A turn's tally starts at nothing. What a tool spends on its behalf is added here, so a
  // receipt can say what the call cost rather than only what the model that asked cost.
  spent_by_tools_ = ToolContext::Spending{};

  ExecutionResult result = RunToolLoop(conversation, turn);
  AddToolSpending(result);
  if (result.has_error) return result;

  if (!result.content.empty()) conversation.Append("assistant", result.content);
  return result;
}

void Executor::AddToolSpending(ExecutionResult& result) const {
  const ToolContext::Spending& spent = spent_by_tools_;
  if (spent.prompt_tokens == 0 && spent.completion_tokens == 0 && !spent.cost) return;
  if (!result.usage) result.usage = TokenUsage{};
  result.usage->prompt_tokens += spent.prompt_tokens;
  result.usage->completion_tokens += spent.completion_tokens;
  result.usage->cached_tokens += spent.cached_tokens;
  // A price adds within one unit and is dropped across two. The turn's own price may already
  // be absent for the same reason, and a mixed total would read as the turn's cost when it is
  // only part of it.
  if (spent.cost) {
    if (!result.usage->cost) {
      result.usage->cost = spent.cost;
    } else if (result.usage->cost->unit == spent.cost->unit) {
      result.usage->cost->amount += spent.cost->amount;
    } else {
      result.usage->cost.reset();
    }
  }
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
  // The tally belongs to the turn: a tool adds to it, and the turn reports it beside what its
  // own model cost.
  tool_ctx.spent = &spent_by_tools_;

  return tool_ctx;
}

void Executor::AppendAssistantTurn(Conversation& conversation, const ChatResult& chat_result) const {
  boost::json::array calls;
  for (const ToolCall& call : chat_result.tool_calls) {
    calls.push_back(session::ToolCallToJson(session::ToolCallRecord{call.id, call.name,
                                                                   call.arguments}));
  }

  ChatMessage assistant_msg;
  assistant_msg.role = kAssistantRole;
  assistant_msg.SetText(chat_result.content);
  assistant_msg.reasoning = chat_result.reasoning;
  assistant_msg.tool_calls = std::move(calls);
  conversation.Append(assistant_msg);
}

ProviderCall Executor::CallProvider(Conversation& conversation, const Turn& turn,
                                    ExecutionResult& result) {
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
    return {.reply = std::move(chat_result), .outcome = ProviderOutcome::kReplied};
  } catch (const std::exception& e) {
    const bool cancelled =
        turn.cancel_token && turn.cancel_token->load(std::memory_order_acquire);
    if (cancelled) {
      spdlog::debug("Request stopped by the caller: {}", e.what());
      return {.reply = {}, .outcome = ProviderOutcome::kStopped};
    }
    if (platform::ShutdownRequested()) {
      spdlog::debug("Request abandoned as the process shut down: {}", e.what());
      return {.reply = {}, .outcome = ProviderOutcome::kAbandoned};
    }
    result.has_error = true;
    const std::string said = text::ReadableError(e.what());
    result.error_message = RefusedForSize(said) ? TooLargeMessage(said, context_window_)
                                                : "Request failed: " + said;
    return {.reply = {}, .outcome = ProviderOutcome::kFailed};
  }
}

// Runs every call of one round. The loop stays sequential -- two `execute_bash` calls in one
// round may be changing the same files, and the tools were written to be called one at a time --
// but the receipts are appended only once the round is over.
//
// That delay is what the provider's prefix cache is paid for. A round's calls all answer the
// same assistant message, so the next request repeats it whichever order the receipts land in;
// appending between two calls would put a message the model has not seen yet in the middle of
// the round, and the next turn would then have to re-read the whole prefix up to that point.
// One round, one block of receipts, one cacheable prefix.
void Executor::RunToolCalls(Conversation& conversation, const Turn& turn,
                            const std::vector<ToolCall>& calls, ExecutionResult& result) {
  ToolContext tool_ctx = BuildToolContext(turn);
  std::vector<ChatMessage> receipts;
  receipts.reserve(calls.size());

  for (const ToolCall& call : calls) {
    if (turn.cancel_token && turn.cancel_token->load(std::memory_order_acquire)) {
      spdlog::debug("Turn cancelled before tool '{}' ran", call.name);
      break;
    }
    // The same stop, one layer out: a deadline that passed while a tool was being called would
    // otherwise only be noticed at the next round, after the tool had already run.
    if (stop_check_ && stop_check_()) {
      spdlog::debug("Turn stopped before tool '{}' ran", call.name);
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
      tool_result = agent_tools::MakeToolResultJson(false, "",
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

    const auto parsed = agent_tools::ParseToolResult(tool_result);
    if (parsed.valid && parsed.blocked) {
      ++result.blocked_tool_calls;
      if (parsed.refusal == agent_tools::Refusal::kNobodyToAsk) ++result.unaskable_tool_calls;
    }

    if (turn.tool_callbacks.on_end) {
      // The envelope travels beside the text rather than inside it: `output` is what the
      // command printed and `page` is how much of it this result is showing, so a reader that
      // only wants the text is not made to unwrap a shape it does not read.
      const boost::json::value page = parsed.valid ? parsed.page : boost::json::value{};
      if (parsed.valid) {
        turn.tool_callbacks.on_end(call.id, call.name, call.arguments, parsed.output, parsed.error,
                                   page);
      } else {
        turn.tool_callbacks.on_end(call.id, call.name, call.arguments, tool_result, "",
                                   boost::json::value{});
      }
    }

    ChatMessage tool_msg;
    tool_msg.role = "tool";
    tool_msg.SetText(std::move(tool_result));
    tool_msg.tool_name = call.name;
    tool_msg.tool_call_id = call.id;
    receipts.push_back(std::move(tool_msg));
  }

  // Appended in the order the calls were made: a provider matches a receipt to its call by
  // `tool_call_id`, and a transcript that listed them backwards would read differently to a
  // person than the round that produced it.
  for (ChatMessage& receipt : receipts) conversation.Append(std::move(receipt));

  // The note rides after every receipt of this round: inside the loop it would land
  // between two tool receipts, and a user message splitting the assistant's
  // tool_calls from their receipts is answered 400 by the gateway on the next round.
  if (!user_note_.empty() && turn.cancel_token) {
    ChatMessage note_msg;
    note_msg.role = "user";
    note_msg.SetText("[user note during the turn] " + user_note_);
    conversation.Append(note_msg);
    user_note_.clear();
  }

  if (turn.cancel_token && turn.cancel_token->load(std::memory_order_acquire)) {
    conversation.SealInterruptedToolCalls();
  }
}

ExecutionResult Executor::RunToolLoop(Conversation& conversation, const Turn& turn) {
  ExecutionResult result;

  for (;;) {
    // A round is counted when it opens rather than when its reply arrives: a turn stopped
    // before asking the model did open that round and did nothing in it, and a count that
    // skipped it would describe a turn shorter than the one the user stopped.
    ++result.rounds;

    // A delegated turn's stop is not one token: the runtime also holds a deadline and knows
    // whether the process is going down, and neither of those has a token to cancel. Asked here
    // so it lands between rounds, which is the only point where a turn can end cleanly.
    if (stop_check_ && stop_check_()) {
      spdlog::debug("Turn stopped before round {} asked anything", result.rounds);
      conversation.SealInterruptedToolCalls();
      return result;
    }

    ProviderCall call = CallProvider(conversation, turn, result);
    if (call.outcome == ProviderOutcome::kFailed) break;
    if (call.outcome == ProviderOutcome::kStopped) {
      conversation.SealInterruptedToolCalls();
      return result;
    }
    if (call.outcome == ProviderOutcome::kAbandoned) {
      return result;
    }
    ChatResult& chat_result = call.reply;

    if (chat_result.tool_calls.empty()) {
      std::string response = chat_result.content;
      const std::string reasoning = llm::ProjectReasoningText(chat_result.reasoning);
      if (response.empty() && !reasoning.empty()) {
        response = reasoning;
        spdlog::debug("Using reasoning as final response (thinking mode)");
      }
      result.content = response;
      result.notice = StopNotice(chat_result.finish_reason);
      // Said only where the stop notice left the line free: a reply cut off at `max_tokens` is
      // a different problem, and the window is not what the reader has to act on then.
      if (result.notice.empty() && result.usage) {
        result.notice = WindowNotice(result.usage->prompt_tokens, context_window_);
      }
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

  return result;
}

}  // namespace pu
