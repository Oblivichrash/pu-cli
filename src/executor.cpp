// SPDX-License-Identifier: GPL-3.0-only
#include "pu/executor.hpp"
#include "pu/infra/platform.hpp"

#include "pu/core/logging.hpp"
#include "pu/tools/tool_result.hpp"

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

#ifdef _WIN32
// Report the Windows kernel/OS version using RtlGetVersion, which returns the
// real version regardless of the application compatibility manifest (unlike
// GetVersionEx, which can be capped). RtlGetVersion is available on every
// supported Windows version, so no deprecated fallback is needed.
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
#endif  // _WIN32

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
#endif  // !_WIN32

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

// Quietly check whether an executable is on PATH. Probing never surfaces
// "command not found" output: on Windows `where /Q` is silent, and on POSIX
// stdout/stderr are redirected to /dev/null, so detection relies on the exit
// code rather than captured text.
bool CommandExists(const std::string& tool) {
  std::string output;
#ifdef _WIN32
  int rc = pu::platform::ExecuteCommand("where /Q \"" + tool + "\"", output);
#else
  int rc = pu::platform::ExecuteCommand(
      "command -v " + tool + " >/dev/null 2>&1", output);
#endif
  return rc == 0;
}

std::string Truncate(const std::string& s, size_t max_len) {
  if (s.size() <= max_len) return s;
  if (max_len <= 3) return s.substr(0, max_len);
  return s.substr(0, max_len - 3) + "...";
}

}  // namespace

std::string Executor::ExtractToolResultContent(const std::string& tool_result) {
  return tools::ExtractToolResultContent(tool_result);
}

void Executor::ProbeStaticEnvironment() {
  if (static_env_info_.probed) return;

  static_env_info_.os_name = OsName();
  static_env_info_.kernel_version = OsKernelVersion();

  static const std::vector<std::string> kTools = {
      "bash", "python3", "gcc", "git", "curl", "jq"};
  for (const auto& tool : kTools) {
    if (CommandExists(tool)) {
      static_env_info_.available_tools.push_back(tool);
    }
  }
  static_env_info_.probed = true;

  spdlog::info("Static environment probed: OS='{}' kernel='{}' tools=[{}]",
               static_env_info_.os_name, static_env_info_.kernel_version,
               [&] {
                 std::string joined;
                 for (size_t i = 0; i < static_env_info_.available_tools.size(); ++i) {
                   if (i > 0) joined += ", ";
                   joined += static_env_info_.available_tools[i];
                 }
                 return joined;
               }());
}

std::string Executor::BuildSystemContextMessage(const Workspace& workspace) const {
  std::ostringstream oss;

  oss << "=== Environment ===\n";
  if (static_env_info_.probed) {
    oss << "OS: " << static_env_info_.os_name << "\n";
    oss << "Kernel: " << static_env_info_.kernel_version << "\n";
    oss << "Available tools: ";
    if (static_env_info_.available_tools.empty()) {
      oss << "(none detected)";
    } else {
      for (size_t i = 0; i < static_env_info_.available_tools.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << static_env_info_.available_tools[i];
      }
    }
    oss << "\n";
  } else {
    oss << "(environment not probed)\n";
  }

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

  auto artifacts = workspace.GetArtifacts();
  std::vector<Artifact> file_artifacts;
  for (const auto& a : artifacts) {
    if (a.type == Artifact::kFilePath) {
      file_artifacts.push_back(a);
    }
  }
  if (file_artifacts.size() > 8) {
    file_artifacts.assign(file_artifacts.end() - 8, file_artifacts.end());
  }
  oss << "=== Known File Paths ===\n";
  if (file_artifacts.empty()) {
    oss << "(none)\n";
  } else {
    for (const auto& fa : file_artifacts) {
      oss << fa.content << "\n";
    }
  }

  auto history = workspace.GetHistory();
  std::vector<const ChatMessage*> tool_msgs;
  for (auto it = history.rbegin(); it != history.rend() && tool_msgs.size() < 2; ++it) {
    if (it->role == "tool") {
      tool_msgs.push_back(&(*it));
    }
  }
  std::reverse(tool_msgs.begin(), tool_msgs.end());

  oss << "=== Recent Tool Executions ===\n";
  if (tool_msgs.empty()) {
    oss << "(none)\n";
  } else {
    for (const auto* tm : tool_msgs) {
      auto tr = tools::ParseToolResult(tm->content);

      oss << "- [" << tm->tool_name << "] ";
      if (tr.valid) {
        if (tr.success) {
          oss << "OK: " << Truncate(tr.stdout_content, 80) << "\n";
        } else {
          oss << "FAILED: " << Truncate(tr.error, 80)
              << " (hint: check the error and retry with a corrected command)\n";
        }
      } else {
        oss << Truncate(tm->content, 80) << "\n";
      }
    }
  }

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
  bool tool_was_called = false;
  const int max_iterations = 20;
  int iteration = 0;
  bool hit_max_iterations = false;

  do {
    if (iteration >= max_iterations) {
      hit_max_iterations = true;
      spdlog::warn("Tool loop reached max_iterations ({}), breaking", max_iterations);
      break;
    }
    ++iteration;
    tool_was_called = false;

    std::vector<ChatMessage> chat_history;
    for (const auto& msg : workspace.GetHistory()) {
      chat_history.push_back(msg);
    }

    std::string system_context = BuildSystemContextMessage(workspace);
    std::string merged_system = system_context;
    auto system_prompt_var = workspace.GetVar("system_prompt");
    if (system_prompt_var && system_prompt_var->is_string() &&
        !system_prompt_var->get<std::string>().empty()) {
      merged_system = system_prompt_var->get<std::string>() + "\n\n" + system_context;
    }

    bool has_system = false;
    for (auto& cm : chat_history) {
      if (cm.role == "system") {
        cm.content = merged_system;
        has_system = true;
        break;
      }
    }
    if (!has_system) {
      ChatMessage sys;
      sys.role = "system";
      sys.content = merged_system;
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
        break;
      }
    } catch (const std::exception& e) {
      result.has_error = true;
      result.error_message = "Request failed: " + std::string(e.what());
      spdlog::error("{}", result.error_message);
      workspace.Append("assistant", result.error_message);
      break;
    }

    if (!collected_calls.empty()) {
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
      }
    }
  } while (tool_was_called);

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

  return result;
}

}  // namespace pu