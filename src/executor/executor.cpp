// SPDX-License-Identifier: GPL-3.0-only
#include "pu/executor/executor.hpp"

#include "pu/renderer.hpp"
#include "pu/core/fork_merge_service.hpp"
#include "tools/command_executor.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <iostream>
#include <sstream>

namespace pu {

using json = nlohmann::json;

Executor::Executor(Toolbox* toolbox)
    : toolbox_(toolbox) {}

void Executor::SetSecurityPolicy(const config::SecurityPolicy& policy) {
  security_policy_ = policy;
}

std::string Executor::Execute(const std::string& input,
                              Workspace& workspace,
                              LLMProvider* provider) {
  workspace.Append("user", input);

  auto tools = toolbox_->GetToolDefinitions();
  if (!provider->SupportsTools() && !tools.empty()) {
    workspace.Compact();
  }

  auto result = RunToolLoop(workspace, provider);

  if (!result.final_response.empty()) {
    workspace.Append("assistant", result.final_response);
  }

  return result.final_response;
}

Executor::ToolLoopResult Executor::RunToolLoop(Workspace& workspace,
                                               LLMProvider* provider) {
  ToolLoopResult result;

  if (!provider->SupportsTools()) {
    result.final_response = "This provider does not support tool calling. Cannot execute tools.";
    return result;
  }

  auto tools = toolbox_->GetToolDefinitions();
  bool tool_was_called = false;
  std::string final_response;

  do {
    tool_was_called = false;

    auto history = workspace.GetHistory();
    std::vector<ChatMessage> chat_history;
    for (const auto& msg : history) {
      chat_history.push_back(msg);
    }

    auto system_prompt_var = workspace.GetVar("system_prompt");
    if (system_prompt_var && system_prompt_var->is_string() && !system_prompt_var->get<std::string>().empty()) {
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
    auto renderer = pu::StreamingRenderer::Create();

    ChatResult chat_result;

    try {
      chat_result = provider->Chat(
        chat_history,
        tools,
        [&](const std::string& token) {
          std::cout << token << std::flush;
          content_stream << token;
        },
        [&](const ToolCall& call) {
          tool_was_called = true;
          collected_calls.push_back(call);
        }
      );

      if (!tool_was_called) {
        final_response = chat_result.content;
        break;
      }
    } catch (const std::exception& e) {
      auto err = "Request failed: " + std::string(e.what());
      spdlog::error("{}", err);
      final_response = err;
      workspace.Append("assistant", final_response);
      break;
    }

    if (!collected_calls.empty()) {
      // Build assistant message with tool_calls, using the ids from the provider.
      ChatMessage assistant_msg;
      assistant_msg.role = "assistant";
      assistant_msg.content = chat_result.content;

      json j_calls = json::array();
      for (const auto& tc : collected_calls) {
        json jc;
        jc["id"] = tc.id;   // use the id from the provider (e.g., from DeepSeek)
        jc["type"] = "function";
        jc["function"]["name"] = tc.name;
        // arguments may already be a string or object; ensure we use the raw arguments as-is
        jc["function"]["arguments"] = tc.arguments;
        j_calls.push_back(jc);
      }
      assistant_msg.tool_calls_json = j_calls.dump();
      assistant_msg.reasoning_content = chat_result.reasoning_content;
      workspace.Append(assistant_msg);

      ToolContext tool_ctx;
      if (security_policy_.has_value()) {
        tool_ctx.security = &security_policy_.value();
      } else {
        static config::SecurityPolicy empty_policy;
        tool_ctx.security = &empty_policy;
        spdlog::warn("No security policy set for Executor. Using empty policy.");
      }

      // Execute each tool and append tool response messages with proper tool_call_id.
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
        tool_msg.tool_name = call.name;      // for informational purposes
        tool_msg.tool_call_id = call.id;     // set the id to match assistant's tool_call
        workspace.Append(tool_msg);
      }
    }
  } while (tool_was_called);

  if (result.tool_call_count > 0 && final_response.empty()) {
    auto history = workspace.GetHistory();
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
      if (it->role == "tool") {
        final_response = it->content;
        break;
      }
    }
    if (final_response.empty()) {
      final_response = "Tool executed successfully.";
    }
  }

  result.final_response = final_response;
  return result;
}

} // namespace pu
