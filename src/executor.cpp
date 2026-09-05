// SPDX-License-Identifier: GPL-3.0-only
#include "pu/executor.hpp"
#include "pu/infra/platform.hpp"

#include "pu/core/logging.hpp"
#include "pu/tools/tool_result.hpp"

#include <boost/json.hpp>
#include "pu/json.hpp"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <winternl.h>
#endif

namespace pu {


namespace {

#ifdef _WIN32
std::string WindowsKernelVersion() {
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
      ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  if (rtl_get_version) {
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtl_get_version(&info) == 0) {
      return std::to_string(info.dwMajorVersion) + "." +
             std::to_string(info.dwMinorVersion) + "." +
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
  while (!output.empty() &&
         (output.back() == '\n' || output.back() == '\r' ||
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

std::string Executor::ExtractToolResultContent(const std::string& tool_result) {
  return tools::ExtractToolResultContent(tool_result);
}

void Executor::ProbeStaticEnvironment() {
  if (static_env_info_.probed) return;

  static_env_info_.os_name = OsName();
  static_env_info_.kernel_version = OsKernelVersion();
  static_env_info_.probed = true;

  spdlog::debug("Probed environment: OS='{}' kernel='{}'",
                static_env_info_.os_name, static_env_info_.kernel_version);
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
  oss << "1. Before calling any tool, output a concise step-by-step plan. Only execute tools after stating the plan.\n";
  oss << "2. When inspecting files, use targeted commands (head -n 50, tail -n 50, grep, sed -n '10,30p') instead of full cat dumps.\n";
  oss << "3. If you need more information from the user, call ask_user and stop. Do not guess.\n";
  oss << "4. Use parallel tool calls when possible to minimize round trips.\n";

  return oss.str();
}

Executor::Executor(Toolbox* toolbox) : toolbox_(toolbox) {
  ProbeStaticEnvironment();
}

void Executor::SetSecurityPolicy(const config::SecurityPolicy& policy) {
  security_policy_ = policy;
}

ExecutionResult Executor::Execute(const std::string& input,
                                  Workspace& workspace,
                                  LLMProvider* provider,
                                  CancelToken cancel_token,
                                  std::function<void(const std::string&)> content_callback) {
  if (!toolbox_) {
    ExecutionResult err;
    err.has_error = true;
    err.error_message = "Tool registry is not initialized.";
    return err;
  }

  workspace.Append("user", input);

  auto tools = toolbox_->GetToolDefinitions();
  if (!provider->SupportsTools() && !tools.empty() && compaction_config_.enabled) {
    if (provider->IsThinkingMode()) {
      spdlog::warn("Compaction is disabled because the provider is in thinking mode. "
                   "Set compaction.enabled=false in agents.json to override.");
    } else {
      workspace.Compact(compaction_config_.keep_head, compaction_config_.keep_tail);
    }
  }

  auto result = RunToolLoop(workspace, provider, cancel_token, content_callback);
  ExecutionResult exec_result;
  if (result.has_error) {
    exec_result.has_error = true;
    exec_result.error_message = result.error_message;
    return exec_result;
  }

  if (!result.final_response.empty() && result.tool_call_count > 0) {
    workspace.Append("assistant", result.final_response);
  }

  exec_result.content = result.final_response;
  exec_result.was_streamed = result.was_streamed;
  exec_result.tool_call_count = result.tool_call_count;
  return exec_result;
}

Executor::ToolLoopResult Executor::RunToolLoop(Workspace& workspace,
                                               LLMProvider* provider,
                                               CancelToken cancel_token,
                                               std::function<void(const std::string&)> content_callback) {
  ToolLoopResult result;
  result.was_streamed = false;

  if (!toolbox_) {
    result.has_error = true;
    result.error_message = "Tool registry is not initialized.";
    return result;
  }

  if (!provider->SupportsTools()) {
    result.final_response = "This provider does not support tool calling. Cannot execute tools.";
    return result;
  }

  auto tools = toolbox_->GetToolDefinitions();
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

    std::vector<ChatMessage> chat_history;
    for (const auto& msg : workspace.GetHistory()) {
      chat_history.push_back(msg);
    }

    bool has_system = false;
    for (const auto& msg : chat_history) {
      if (msg.role == "system") {
        has_system = true;
        break;
      }
    }

    if (!has_system) {
      std::string static_context = BuildStaticSystemContext();
      auto system_prompt_var = workspace.GetVar("system_prompt");
      if (system_prompt_var && system_prompt_var->is_string() &&
          !boost::json::value_to<std::string>(*system_prompt_var).empty()) {
        static_context = boost::json::value_to<std::string>(*system_prompt_var) + "\n\n" + static_context;
      }
      ChatMessage sys;
      sys.role = "system";
      sys.content = static_context;
      chat_history.insert(chat_history.begin(), std::move(sys));
    }

    std::vector<ToolCall> collected_calls;
    std::ostringstream content_stream;
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
              content_stream << token;
            }
          },
          [&](const ToolCall& call) {
            tool_was_called = true;
            collected_calls.push_back(call);
          },
          cancel_token);

      if (!tool_was_called) {
        std::string response = chat_result.content;
        if (response.empty() && !chat_result.reasoning_content.empty()) {
          response = chat_result.reasoning_content;
          spdlog::debug("Using reasoning_content as final response (thinking mode)");
        }
        result.final_response = response;
        break;
      }
    } catch (const std::exception& e) {
      result.has_error = true;
      result.error_message = "Request failed: " + std::string(e.what());
      spdlog::error("{}", result.error_message);
      workspace.Append("assistant", result.error_message);
      break;
    }

    for (const auto& call : collected_calls) {
      if (call.name == "ask_user") {
        result.final_response = json::ValueOrDefault<std::string>(call.arguments, "question", "");
        result.completed = true;
        result.was_streamed = false;
        return result;
      }
    }

    for (auto& tc : collected_calls) {
      if (tc.id.empty()) {
        tc.id = "call_" + std::to_string(++next_tool_call_id_);
      }
    }

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.content = chat_result.content;
    assistant_msg.reasoning_content = chat_result.reasoning_content;

    boost::json::array j_calls;
    for (const auto& tc : collected_calls) {
      boost::json::value jc = {
        {"id", tc.id},
        {"type", "function"},
        {"function", {
          {"name", tc.name},
          {"arguments", tc.arguments},
        }},
      };
      j_calls.push_back(jc);
    }
    assistant_msg.tool_calls_json = boost::json::serialize(j_calls);
    workspace.Append(assistant_msg);

    ToolContext tool_ctx;
    if (security_policy_.has_value()) {
      tool_ctx.security = &security_policy_.value();
    } else {
      static config::SecurityPolicy empty_policy;
      tool_ctx.security = &empty_policy;
      spdlog::warn("No security policy set for Executor. Using empty policy.");
    }
    if (!tool_ctx.request_confirmation) {
      tool_ctx.request_confirmation = [](const std::string&) { return true; };
    }

    for (const auto& call : collected_calls) {
      if (call.name.empty()) {
        spdlog::warn("Skipping tool call with empty name");
        continue;
      }
      ++result.tool_call_count;
      std::string tool_result;
      SetLogToolName(call.name);
      auto tool_start = std::chrono::steady_clock::now();
      try {
        tool_result = toolbox_->ExecuteTool(call.name, call.arguments, tool_ctx);
      } catch (const std::exception& e) {
        tool_result = std::string("Tool execution error: ") + e.what();
      }
      auto tool_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - tool_start).count();
      SetLogDurationMs(tool_ms);
      spdlog::info("Tool '{}' completed in {} ms", call.name, tool_ms);
      ClearLogToolName();
      ClearLogDurationMs();

      ChatMessage tool_msg;
      tool_msg.role = "tool";
      tool_msg.content = tool_result;
      tool_msg.tool_name = call.name;
      tool_msg.tool_call_id = call.id;
      workspace.Append(tool_msg);
    }

  } while (tool_was_called);

  if (hit_max_iterations && result.final_response.empty()) {
    result.final_response =
        "Tool execution reached the maximum number of iterations without generating a final answer. "
        "Please rephrase your request or narrow the scope.";
    result.has_error = true;
    spdlog::error("{}", result.final_response);
    return result;
  }

  if (result.final_response.empty() && result.tool_call_count == 0) {
    result.has_error = true;
    result.error_message = "Model returned an empty response without any tool calls. "
                           "Please check the backend service or try again.";
    spdlog::error("{}", result.error_message);
    return result;
  }

  if (result.final_response.empty() && result.tool_call_count > 0) {
    spdlog::info("Tool execution completed without a final text response – considered successful.");
  }

  return result;
}

}  // namespace pu
