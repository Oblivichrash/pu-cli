// SPDX-License-Identifier: GPL-3.0-only
#include "pu/executor.hpp"

#include "pu/core/platform.hpp"
#include "pu/core/logging.hpp"
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
    return "The reply stopped at the token limit, so it may be incomplete. Raise "
           "max_tokens for this backend to get the rest.";
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

Executor::Executor() { ProbeStaticEnvironment(); }

void Executor::SetSecurityPolicy(const config::SecurityPolicy& policy) {
  security_policy_ = policy;
}

ExecutionResult Executor::Execute(const std::string& input, Conversation& conversation,
                                  LLMProvider* provider, Toolbox* toolbox,
                                  CancelToken cancel_token,
                                  std::function<void(const std::string&)> content_callback,
                                  ToolCallbacks tool_callbacks,
                                  std::function<void(const std::string&)> reasoning_callback) {
  if (toolbox == nullptr) {
    ExecutionResult err;
    err.has_error = true;
    err.error_message = "Tool registry is not initialized.";
    return err;
  }

  conversation.Append("user", input);

  ExecutionResult result = RunToolLoop(conversation, provider, toolbox, cancel_token,
                                       content_callback, tool_callbacks, reasoning_callback);
  if (result.has_error) return result;

  if (!result.content.empty()) conversation.Append("assistant", result.content);
  return result;
}

ExecutionResult Executor::RunToolLoop(Conversation& conversation, LLMProvider* provider,
                                      Toolbox* toolbox, CancelToken cancel_token,
                                      std::function<void(const std::string&)> content_callback,
                                      ToolCallbacks tool_callbacks,
                                      std::function<void(const std::string&)> reasoning_callback) {
  ExecutionResult result;

  if (!provider->SupportsTools()) {
    result.content = "This provider does not support tool calling. Cannot execute tools.";
    return result;
  }

  auto tools = toolbox->GetToolDefinitions();
  const int max_iterations = 20;
  int iteration = 0;
  bool hit_max_iterations = false;
  bool tool_was_called = false;

  do {
    tool_was_called = false;
    if (iteration >= max_iterations) {
      hit_max_iterations = true;
      spdlog::warn("Tool loop reached max_iterations ({}), breaking", max_iterations);
      break;
    }
    ++iteration;

    session::RequestInputs inputs;
    inputs.system_prompt = system_prompt_;
    inputs.environment = BuildStaticSystemContext();

    std::vector<ChatMessage> chat_history =
        session::BuildRequestPath(conversation.GetGraph(), inputs);

    ChatResult chat_result;

    try {
      chat_result = provider->Chat(
          chat_history, tools,
          [&](const std::string& token) {
            if (!token.empty()) {
              result.was_streamed = true;
              if (content_callback) {
                content_callback(token);  // web mode: push SSE, stay silent
              } else {
                std::cout << token << std::flush;  // CLI typewriter
              }
            }
          },
          cancel_token, reasoning_callback);

      if (chat_result.usage) {
        spdlog::debug("tokens: prompt={}, completion={}", chat_result.usage->prompt_tokens,
                      chat_result.usage->completion_tokens);
      }

      tool_was_called = !chat_result.tool_calls.empty();
      if (!tool_was_called) {
        std::string response = chat_result.content;
        if (response.empty() && !chat_result.reasoning_content.empty()) {
          response = chat_result.reasoning_content;
          spdlog::debug("Using reasoning_content as final response (thinking mode)");
        }
        result.content = response;
        result.notice = StopNotice(chat_result.finish_reason);
        if (!chat_result.model.empty()) {
          result.model = chat_result.model;
          spdlog::debug("Answered by {}", chat_result.model);
        }
        break;
      }
    } catch (const std::exception& e) {
      if ((cancel_token && cancel_token->load(std::memory_order_acquire)) ||
          platform::IsInterrupted()) {
        spdlog::debug("Request stopped by the caller: {}", e.what());
        return result;
      }
      result.has_error = true;
      result.error_message = "Request failed: " + std::string(e.what());
      spdlog::error("{}", result.error_message);
      break;
    }

    for (auto& tc : chat_result.tool_calls) {
      if (tc.id.empty()) {
        tc.id = "call_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                std::to_string(++next_tool_call_id_);
      }
    }

    ChatMessage assistant_msg;
    assistant_msg.role = context::kAssistantRole;
    assistant_msg.content = chat_result.content;
    assistant_msg.reasoning_content = chat_result.reasoning_content;

    boost::json::array j_calls;
    for (const auto& tc : chat_result.tool_calls) {
      j_calls.push_back(
          context::ToolCallToJson(context::ToolCallRecord{tc.id, tc.name, tc.arguments}));
    }
    assistant_msg.tool_calls = std::move(j_calls);
    conversation.Append(assistant_msg);

    ToolContext tool_ctx;
    if (security_policy_.has_value()) {
      tool_ctx.security = &security_policy_.value();
    } else {
      static config::SecurityPolicy empty_policy;
      tool_ctx.security = &empty_policy;
      spdlog::warn("No security policy set for Executor. Using empty policy.");
    }
    for (const auto& call : chat_result.tool_calls) {
      ++result.tool_call_count;

      if (tool_callbacks.on_start) {
        tool_callbacks.on_start(call.id, call.name, call.arguments);
      }

      std::string tool_result;
      SetLogToolName(call.name);
      auto tool_start = std::chrono::steady_clock::now();
      try {
        tool_result = toolbox->ExecuteTool(call.name, call.arguments, tool_ctx);
      } catch (const std::exception& e) {
        tool_result = tools::MakeToolResultJson(
            false, "", "", std::string("Tool execution error: ") + e.what(), -1);
      }
      auto tool_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - tool_start)
                         .count();
      SetLogDurationMs(tool_ms);
      spdlog::info("Tool '{}' completed in {} ms", call.name, tool_ms);
      ClearLogToolName();
      ClearLogDurationMs();

      if (tool_callbacks.on_end) {
        auto parsed = tools::ParseToolResult(tool_result);
        if (parsed.valid) {
          tool_callbacks.on_end(call.id, parsed.stdout_content, parsed.error);
        } else {
          tool_callbacks.on_end(call.id, tool_result, "");
        }
      }

      ChatMessage tool_msg;
      tool_msg.role = "tool";
      tool_msg.content = tool_result;
      tool_msg.tool_name = call.name;
      tool_msg.tool_call_id = call.id;
      conversation.Append(tool_msg);
    }

  } while (tool_was_called);

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
