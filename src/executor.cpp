// SPDX-License-Identifier: GPL-3.0-only
#include "pu/executor.hpp"
#include "pu/infra/platform.hpp"

#include "pu/core/logging.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <winternl.h>
#endif

namespace pu {

using json = nlohmann::json;

namespace {
constexpr int kMaxIterations = 20;

#ifdef _WIN32
// RtlGetVersion bypasses the manifest compatibility layer and reports the true OS version.
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
                                  LLMProvider* provider) {
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

  auto result = RunToolLoop(workspace, provider);
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
                                               LLMProvider* provider) {
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
  bool should_continue = true;
  int iteration = 0;

  while (should_continue && iteration < kMaxIterations) {
    ++iteration;
    bool tool_was_called = false;

    std::vector<ChatMessage> chat_history = PrepareChatHistory(workspace);

    std::vector<ToolCall> collected_calls;
    std::ostringstream content_stream;
    ChatResult chat_result;

    try {
      chat_result = provider->Chat(
          chat_history, tools,
          [&](const std::string& token) {
            if (!token.empty()) {
              result.was_streamed = true;
              std::cout << token << std::flush;
              content_stream << token;
            }
          },
          [&](const ToolCall& call) {
            tool_was_called = true;
            collected_calls.push_back(call);
          });

      if (!tool_was_called) {
        result.final_response = chat_result.content;
        should_continue = false;
      }
    } catch (const std::exception& e) {
      result.has_error = true;
      result.error_message = "Request failed: " + std::string(e.what());
      spdlog::error("{}", result.error_message);
      workspace.Append("assistant", result.error_message);
      should_continue = false;
    }

    if (should_continue) {
      should_continue = ProcessToolCalls(workspace, chat_result, collected_calls, result);
    }
  }

  // Natural exit with the counter at the cap means the loop ran out of budget.
  bool hit_max_iterations = (iteration == kMaxIterations);
  if (hit_max_iterations) {
    spdlog::warn("Tool loop reached max_iterations ({}), breaking", kMaxIterations);
  }

  FinalizeResult(workspace, result, hit_max_iterations);

  return result;
}

// Builds the chat history for the next provider call, injecting the static
// system context when the transcript has no system message yet.
std::vector<ChatMessage> Executor::PrepareChatHistory(Workspace& workspace) const {
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
        !system_prompt_var->get<std::string>().empty()) {
      static_context = system_prompt_var->get<std::string>() + "\n\n" + static_context;
    }
    ChatMessage sys;
    sys.role = "system";
    sys.content = static_context;
    chat_history.insert(chat_history.begin(), std::move(sys));
  }

  return chat_history;
}

// Records the assistant turn, executes every collected tool call, appends the
// tool results to the transcript, and stops early when ask_user asks a
// clarification question. Returns true to continue the tool loop, false to stop.
bool Executor::ProcessToolCalls(Workspace& workspace,
                                const ChatResult& chat_result,
                                std::vector<ToolCall>& collected_calls,
                                ToolLoopResult& result) {
  if (collected_calls.empty()) return false;

  for (auto& tc : collected_calls) {
    if (tc.id.empty()) {
      tc.id = "call_" + std::to_string(++next_tool_call_id_);
    }
  }

  ChatMessage assistant_msg;
  assistant_msg.role = "assistant";
  assistant_msg.content = chat_result.content;
  assistant_msg.reasoning_content = chat_result.reasoning_content;

  json j_calls = json::array();
  for (const auto& tc : collected_calls) {
    json jc;
    jc["id"] = tc.id;
    jc["type"] = "function";
    jc["function"]["name"] = tc.name;
    jc["function"]["arguments"] = tc.arguments;
    j_calls.push_back(jc);
  }
  assistant_msg.tool_calls_json = j_calls.dump();
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

    // ask_user is a clarification request, not a tool outcome: surface the
    // question as the final response and stop the loop. The exchange is
    // already in the transcript (assistant tool call + tool result), and
    // the question is appended as the assistant turn by the caller.
    if (call.name == "ask_user") {
      try {
        auto j = json::parse(tool_result);
        if (j.value("error", "") == "clarification_needed") {
          result.final_response = j.value("question", "");
          result.was_streamed = false;
          return false;
        }
      } catch (const std::exception&) {
        // Malformed result: let the model see the tool error and retry.
      }
    }
  }

  return true;
}
// Falls back to the last non-empty assistant turn in the transcript when the
// provider returned no final response, or emits a generic message.
void Executor::FinalizeResult(Workspace& workspace, ToolLoopResult& result,
                              bool hit_max_iterations) const {
  if (result.final_response.empty()) {
    auto history = workspace.GetHistory();
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
      if (it->role == "assistant" && !it->content.empty()) {
        result.final_response = it->content;
        break;
      }
    }
  }

  if (result.final_response.empty() && !result.has_error) {
    if (hit_max_iterations) {
      result.final_response =
          "Tool execution reached the maximum number of iterations without generating a final answer. "
          "Please rephrase your request or narrow the scope.";
    } else {
      result.final_response =
          "Tool execution completed but no final answer was generated. Please rephrase your request.";
    }
  }
}

}  // namespace pu
