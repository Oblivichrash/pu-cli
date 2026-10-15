// SPDX-License-Identifier: GPL-3.0-only
#include "pu/executor.hpp"

#include "pu/core/platform.hpp"
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
  conversation.Append("user", input);

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
  return tool_ctx;
}

void Executor::AppendAssistantTurn(Conversation& conversation, const ChatResult& chat_result) const {
  boost::json::array calls;
  for (const ToolCall& call : chat_result.tool_calls) {
    calls.push_back(context::ToolCallToJson(context::ToolCallRecord{call.id, call.name,
                                                                   call.arguments}));
  }

  ChatMessage assistant_msg;
  assistant_msg.role = context::kAssistantRole;
  assistant_msg.content = chat_result.content;
  assistant_msg.reasoning = chat_result.reasoning;
  assistant_msg.tool_calls = std::move(calls);
  conversation.Append(assistant_msg);
}

ChatResult Executor::CallProvider(Conversation& conversation, const Turn& turn,
                                  ExecutionResult& result, bool& failed, bool& stopped) {
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
    if (turn.content_callback) {
      turn.content_callback(token);
    } else {
      std::cout << token << std::flush;
    }
  };

  try {
    ChatResult chat_result = turn.provider->Chat(request);
    if (chat_result.usage) {
      result.usage = chat_result.usage;
      spdlog::debug("tokens: prompt={}, completion={}, total={}, cached={}, credit={}",
                    chat_result.usage->prompt_tokens, chat_result.usage->completion_tokens,
                    chat_result.usage->total_tokens, chat_result.usage->cached_tokens,
                    chat_result.usage->credit);
    }
    if (!chat_result.raw_usage.is_null()) {
      spdlog::debug("usage as the provider reported it: {}", json::serialize(chat_result.raw_usage));
    }
    if (!chat_result.unread_fields.empty()) {
      spdlog::debug("fields on the reply nothing reads: {}",
                    boost::json::serialize(boost::json::array(chat_result.unread_fields.begin(),
                                                              chat_result.unread_fields.end())));
    }
    return chat_result;
  } catch (const std::exception& e) {
    if ((turn.cancel_token && turn.cancel_token->load(std::memory_order_acquire)) ||
        platform::IsInterrupted()) {
      spdlog::debug("Request stopped by the caller: {}", e.what());
      stopped = true;
      return {};
    }
    failed = true;
    result.has_error = true;
    result.error_message = "Request failed: " + std::string(e.what());
    spdlog::error("{}", result.error_message);
    return {};
  }
}

void Executor::RunToolCalls(Conversation& conversation, const Turn& turn,
                            const std::vector<ToolCall>& calls, ExecutionResult& result) {
  ToolContext tool_ctx = BuildToolContext(turn);

  for (const ToolCall& call : calls) {
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

    if (turn.tool_callbacks.on_end) {
      const auto parsed = tools::ParseToolResult(tool_result);
      if (parsed.valid) {
        turn.tool_callbacks.on_end(call.id, parsed.output, parsed.error);
      } else {
        turn.tool_callbacks.on_end(call.id, tool_result, "");
      }
    }

    if (!user_note_.empty() && turn.cancel_token) {
      ChatMessage note_msg;
      note_msg.role = "user";
      note_msg.content = "[user note during the turn] " + user_note_;
      conversation.Append(note_msg);
      user_note_.clear();
    }

    ChatMessage tool_msg;
    tool_msg.role = "tool";
    tool_msg.content = tool_result;
    tool_msg.tool_name = call.name;
    tool_msg.tool_call_id = call.id;
    conversation.Append(tool_msg);
  }
}

ExecutionResult Executor::RunToolLoop(Conversation& conversation, const Turn& turn) {
  ExecutionResult result;
  bool hit_max_iterations = false;

  for (int iteration = 0;; ++iteration) {
    if (iteration >= kMaxToolIterations) {
      hit_max_iterations = true;
      spdlog::warn("Tool loop reached max_iterations ({}), breaking", kMaxToolIterations);
      break;
    }

    bool failed = false;
    bool stopped = false;
    ChatResult chat_result = CallProvider(conversation, turn, result, failed, stopped);
    if (failed) break;
    if (stopped) return result;

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

  if (hit_max_iterations && result.content.empty()) {
    result.content =
        "Tool execution reached the maximum number of iterations without generating a final "
        "answer. "
        "Please rephrase your request or narrow the scope.";
    result.has_error = true;
    spdlog::error("{}", result.content);
    return result;
  }

  if (!result.has_error && result.content.empty() && result.tool_call_count == 0) {
    result.has_error = true;
    result.error_message =
        "Model returned an empty response without any tool calls. "
        "Please check the backend service or try again.";
    spdlog::error("{}", result.error_message);
    return result;
  }

  if (result.content.empty() && result.tool_call_count > 0) {
    spdlog::info("Tool execution completed without a final text response - considered successful.");
  }

  return result;
}

}  // namespace pu
