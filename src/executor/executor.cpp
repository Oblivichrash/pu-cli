// SPDX-License-Identifier: GPL-3.0-only
#include "pu/executor/executor.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <iostream>
#include <sstream>

#include "tools/command_executor.hpp"

namespace pu {

using json = nlohmann::json;

Executor::Executor(Toolbox* toolbox) : toolbox_(toolbox) {}

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

    auto system_prompt_var = workspace.GetVar("system_prompt");
    if (system_prompt_var && system_prompt_var->is_string() &&
        !system_prompt_var->get<std::string>().empty()) {
      bool has_system = false;
      for (const auto& cm : chat_history) {
        if (cm.role == "system") {
          has_system = true;
          break;
        }
      }
      if (!has_system) {
        ChatMessage sys;
        sys.role = "system";
        sys.content = system_prompt_var->get<std::string>();
        chat_history.insert(chat_history.begin(), std::move(sys));
      }
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
        try {
          tool_result = toolbox_->ExecuteTool(call.name, call.arguments, tool_ctx);
        } catch (const std::exception& e) {
          tool_result = std::string("Tool execution error: ") + e.what();
        }

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
