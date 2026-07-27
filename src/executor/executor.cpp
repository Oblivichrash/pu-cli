// SPDX-License-Identifier: GPL-3.0-only
#include "pu/executor/executor.hpp"

#include "pu/renderer.hpp"
#include "pu/core/fork_merge_service.hpp"
#include "tools/command_executor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <sstream>

namespace pu {

using json = nlohmann::json;

Executor::Executor(Toolbox* toolbox)
    : toolbox_(toolbox) {}

std::string Executor::Execute(const std::string& input,
                              Workspace& workspace,
                              LLMProvider* provider) {
  // 1. Append user message to workspace
  workspace.Append("user", input);

  // 2. Check provider supports tools
  auto tools = toolbox_->GetToolDefinitions();
  if (!provider->SupportsTools() && !tools.empty()) {
    workspace.Compact();
  }

  // 3. Run tool loop
  auto result = RunToolLoop(workspace, provider);

  // 4. Append final response to workspace
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

    // Get history from workspace
    auto history = workspace.GetHistory();

    // Build ChatMessage history for the provider
    std::vector<ChatMessage> chat_history;
    for (const auto& msg : history) {
      chat_history.push_back(msg);
    }

    // Check for system prompt in workspace variables
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

    // Check show_reasoning
    bool show_reasoning = false;
    auto show_reasoning_var = workspace.GetVar("show_reasoning");
    if (show_reasoning_var && show_reasoning_var->is_boolean()) {
      show_reasoning = show_reasoning_var->get<bool>();
    }

    std::vector<ToolCall> collected_calls;
    std::ostringstream content_stream;
    auto renderer = pu::StreamingRenderer::Create(show_reasoning);

    try {
      auto chat_result = provider->Chat(
        chat_history,
        tools,
        [&](const std::string& token) {
          // For rendering - we use the old-style callback
          // Since StreamingRenderer expects backend::ChatCallback, we adapt
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
      std::cerr << "\nError: " << err << '\n';
      final_response = err;
      workspace.Append("assistant", final_response);
      break;
    }

    // Add assistant message with tool calls to workspace
    if (!collected_calls.empty()) {
      ChatMessage assistant_msg;
      assistant_msg.role = "assistant";
      assistant_msg.content = "";

      // Serialize tool_calls to JSON
      json j_calls = json::array();
      for (const auto& tc : collected_calls) {
        json jc;
        jc["name"] = tc.name;
        if (tc.arguments.is_string()) {
          jc["arguments"] = tc.arguments.get<std::string>();
        } else {
          jc["arguments"] = tc.arguments;
        }
        j_calls.push_back(jc);
      }
      assistant_msg.tool_calls_json = j_calls.dump();
      workspace.Append(assistant_msg);
    }

    // Execute each tool
    ToolContext tool_ctx;
    auto security_policy_var = workspace.GetVar("security_policy");
    // Security context is handled outside for now

    for (const auto& call : collected_calls) {
      std::string tool_result;
      try {
        tool_result = toolbox_->ExecuteTool(call.name, call.arguments, tool_ctx);
      } catch (const std::exception& e) {
        tool_result = std::string("Tool execution error: ") + e.what();
      }

      ChatMessage tool_msg;
      tool_msg.role = "tool_result";
      tool_msg.content = tool_result;
      tool_msg.tool_name = call.name;
      workspace.Append(tool_msg);
    }
  } while (tool_was_called);

  result.final_response = final_response;
  return result;
}

}  // namespace pu
