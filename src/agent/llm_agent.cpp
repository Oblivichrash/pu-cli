// SPDX-License-Identifier: GPL-3.0-only
#include "agent/llm_agent.hpp"
#include "pu/core/context.hpp"
#include "tools/command_executor.hpp"

#include "pu/renderer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>

namespace pu::agents {

LLMAgent::LLMAgent(const std::string& name,
                   std::unique_ptr<backend::Backend> backend,
                   std::unique_ptr<agent::ToolRegistry> tool_registry,
                   const agent::config::SecurityPolicy& security)
    : name_(name), backend_(std::move(backend)), tool_registry_(std::move(tool_registry)),
      security_(security) {
  ReloadExternalTools();
}

void LLMAgent::ResetSession() {
  history_.clear();
}

std::vector<backend::Message> LLMAgent::BuildInitialHistory() const {
  std::vector<backend::Message> initial;
  for (const auto& cm : history_) {
    backend::Message msg;
    if (cm.role == "user") {
      msg.role = backend::Message::Role::kUser;
    } else if (cm.role == "assistant" || cm.role == name_) {
      msg.role = backend::Message::Role::kAssistant;
    } else if (cm.role == "system") {
      msg.role = backend::Message::Role::kSystem;
    } else if (cm.role == "tool_result") {
      msg.role = backend::Message::Role::kTool;
    } else {
      msg.role = backend::Message::Role::kSystem;
    }
    msg.content = cm.content;
    msg.tool_name = cm.tool_name;
    initial.push_back(msg);
  }
  return initial;
}

void LLMAgent::AppendTurnToHistory(const std::vector<backend::Message>& history,
                                   size_t initial_size,
                                   std::vector<ChatMessage>& turn_history) const {
  for (size_t i = initial_size + 1; i < history.size(); ++i) {
    const auto& msg = history[i];
    ChatMessage cm;
    cm.id = 0;
    cm.timestamp = "";
    if (msg.role == backend::Message::Role::kAssistant) {
      cm.role = name_;
      if (!msg.content.empty()) {
        cm.content = msg.content;
      } else {
        std::ostringstream tool_summary;
        for (const auto& tc : msg.tool_calls) {
          tool_summary << "[ToolCall: " << tc.name << "(" << tc.arguments << ")]";
        }
        cm.content = tool_summary.str();
      }
    } else if (msg.role == backend::Message::Role::kTool) {
      cm.role = "tool_result";
      cm.content = msg.content;
      cm.tool_name = msg.tool_name;
    } else {
      cm.role = "unknown";
      cm.content = msg.content;
    }
    turn_history.push_back(cm);
  }
}

std::string LLMAgent::Handle(const std::string& input, agent::AgentContext& ctx) {

  auto system_prompt_var = ctx.context->GetVar("system_prompt");
  if (system_prompt_var && system_prompt_var->is_string() && !system_prompt_var->get<std::string>().empty()) {
    std::string prompt = system_prompt_var->get<std::string>();
    bool has_system = false;
    for (const auto& cm : history_) {
      if (cm.role == "system") {
        has_system = true;
        break;
      }
    }
    if (!has_system) {
      history_.insert(history_.begin(),
                      {0, "", "system", prompt, ""});
    }
  }

  history_.push_back({0, "", "user", input, ""});

  auto initial = BuildInitialHistory();
  std::vector<ChatMessage> turn_history;


  bool show_reasoning = false;
  auto show_reasoning_var = ctx.context->GetVar("show_reasoning");
  if (show_reasoning_var && show_reasoning_var->is_boolean()) {
    show_reasoning = show_reasoning_var->get<bool>();
  }

  auto response = RunToolLoop(input, show_reasoning, turn_history, ctx, initial);
  history_.insert(history_.end(), turn_history.begin(), turn_history.end());
  return response;
}

std::string LLMAgent::RunToolLoop([[maybe_unused]] const std::string& user_input,
                                  bool show_reasoning,
                                  std::vector<ChatMessage>& turn_history,
                                  agent::AgentContext& ctx,
                                  const std::vector<backend::Message>& initial_history) {
  if (!backend_->SupportsTools()) {
    return "This backend does not support tool calling. Cannot execute tools.";
  }

  auto history = initial_history;

  auto tools = tool_registry_->GetToolDefinitions();
  std::string final_response;
  bool tool_was_called = false;

  do {
    tool_was_called = false;
    std::vector<backend::ToolCall> collected_calls;
    std::ostringstream content_stream;
    auto renderer = pu::StreamingRenderer::Create(show_reasoning);

    try {
      backend_->Chat(history, tools,
        [&](backend::TokenType type, std::string_view token, bool is_final) {
          if (type == backend::TokenType::kContent) {
            renderer(type, token, is_final);
            if (!is_final) content_stream << token;
          } else if (type == backend::TokenType::kReasoning) {
            renderer(type, token, is_final);
          }
        },
        [&](const backend::ToolCall& call) {
          tool_was_called = true;
          collected_calls.push_back(call);
        });
    } catch (const std::exception& e) {
      auto err = "Request failed: " + std::string(e.what());
      std::cerr << "\nError: " << err << '\n';
      final_response = err;
      turn_history.push_back({0, "", name_, final_response, ""});
      break;
    }

    if (!tool_was_called) {
      final_response = content_stream.str();
      turn_history.push_back({0, "", name_, final_response, ""});
      break;
    }

    backend::Message assistant_msg;
    assistant_msg.role = backend::Message::Role::kAssistant;
    assistant_msg.tool_calls = collected_calls;
    history.push_back(assistant_msg);

    turn_history.push_back({0, "", name_, "", ""});

    agent::ToolContext tool_ctx;
    tool_ctx.security = &security_;
    tool_ctx.request_confirmation = [this](const std::string& message) -> bool {
      if (!confirmation_callback_) return true;
      agent::ConfirmationRequest req;
      req.description = message;
      req.highest_risk = pu::executor::RiskLevel::kNeutral;
      auto choice = confirmation_callback_(req);
      return (choice == agent::ConfirmationChoice::kApproveOnce ||
              choice == agent::ConfirmationChoice::kApproveAllSafe);
    };

    for (const auto& call : collected_calls) {
      std::string result;
      try {
        auto args = nlohmann::json::parse(call.arguments);
        result = tool_registry_->ExecuteTool(call.name, args, tool_ctx);
      } catch (const std::exception& e) {
        result = std::string("Argument parse error: ") + e.what();
      }

      backend::Message tool_msg;
      tool_msg.role = backend::Message::Role::kTool;
      tool_msg.tool_name = call.id;
      tool_msg.content = result;
      history.push_back(tool_msg);
      turn_history.push_back({0, "", "tool_result", result, call.id});
    }
  } while (tool_was_called);

  AppendTurnToHistory(history, initial_history.size(), turn_history);
  return final_response;
}

std::vector<ChatMessage> LLMAgent::SaveState() const {
  return history_;
}

void LLMAgent::LoadState(const std::vector<ChatMessage>& messages) {
  history_ = messages;
}

void LLMAgent::ReloadExternalTools() {
  const char* home = std::getenv("HOME");
  if (!home) return;
  auto tools_dir = std::filesystem::path(home) / ".pu" / "tools";
  if (tool_registry_) {
    tool_registry_->ReloadExternalTools(tools_dir.string());
  }
}

}  // namespace pu::agents
