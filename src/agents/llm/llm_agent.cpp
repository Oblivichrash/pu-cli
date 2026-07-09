// SPDX-License-Identifier: GPL-3.0-only
#include "agents/llm/llm_agent.hpp"
#include "pu/renderer.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <filesystem>

namespace pu::agents {

LLMAgent::LLMAgent(const std::string& name,
                   std::unique_ptr<backend::Backend> backend,
                   std::unique_ptr<agent::ToolRegistry> tool_registry,
                   const config::SecurityPolicy& security)
    : name_(name), backend_(std::move(backend)), tool_registry_(std::move(tool_registry)),
      security_(security) {
  ReloadExternalTools();
}

void LLMAgent::ResetSession() {
  history_.clear();
  recent_scores_.clear();
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
  if (ctx.system_prompt && !ctx.system_prompt->empty()) {
    bool has_system = false;
    for (const auto& cm : history_) {
      if (cm.role == "system") {
        has_system = true;
        break;
      }
    }
    if (!has_system) {
      history_.insert(history_.begin(),
                      {0, "", "system", *ctx.system_prompt, ""});
    }
  }

  history_.push_back({0, "", "user", input, ""});

  auto initial = BuildInitialHistory();
  std::vector<ChatMessage> turn_history;
  auto response = RunToolLoop(input, ctx.show_reasoning, turn_history, ctx, initial);
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
  // Note: user_input is already in initial_history (added in Handle()).
  // Do NOT push another user message here.

  auto tools = tool_registry_->GetToolDefinitions();
  std::string final_response;
  bool tool_was_called = false;

  do {
    tool_was_called = false;
    std::vector<backend::ToolCall> collected_calls;
    std::ostringstream content_stream;
    auto renderer = pu::StreamingRenderer::Create(show_reasoning);

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

    if (!tool_was_called) {
      final_response = content_stream.str();
      turn_history.push_back({0, "", name_, final_response, ""});
      break;
    }

    // Push assistant message with tool_calls BEFORE executing tools
    backend::Message assistant_msg;
    assistant_msg.role = backend::Message::Role::kAssistant;
    assistant_msg.tool_calls = collected_calls;
    history.push_back(assistant_msg);

    // Also record in turn_history (without content)
    turn_history.push_back({0, "", name_, "", ""});

    agent::ToolContext tool_ctx;
    tool_ctx.security = &security_;
    tool_ctx.request_confirmation = [&ctx](const std::string& message) -> bool {
      pu::agent::ConfirmationRequest req;
      req.description = message;
      req.highest_risk = pu::executor::RiskLevel::kNeutral;
      auto choice = ctx.request_confirmation(req);
      return (choice == pu::agent::ConfirmationChoice::kApproveOnce ||
              choice == pu::agent::ConfirmationChoice::kApproveAllSafe);
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

double LLMAgent::EvaluateRelevance(const ChatMessage& msg) {
  std::string lower = msg.content;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  double score = 0.0;
  if (lower.find("error") != std::string::npos) score += 0.4;
  if (lower.find("fail") != std::string::npos) score += 0.4;
  if (lower.find("urgent") != std::string::npos) score += 0.5;
  if (lower.find("crash") != std::string::npos) score += 0.5;
  if (lower.find("timeout") != std::string::npos) score += 0.3;
  return std::min(score, 1.0);
}

void LLMAgent::OnPanelMessage(const ChatMessage& msg) {
  double s = EvaluateRelevance(msg);
  if (s > 0.0) recent_scores_.push_back(s);
}

std::optional<std::string> LLMAgent::ProactiveReply() {
  if (std::any_of(recent_scores_.begin(), recent_scores_.end(),
                  [this](double s) { return s >= proactive_threshold_; })) {
    recent_scores_.clear();
    return "I noticed a possible error. Reply @" + name_ + " to investigate.";
  }
  return std::nullopt;
}

void LLMAgent::SetProactiveThreshold(double threshold) {
  proactive_threshold_ = threshold;
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
