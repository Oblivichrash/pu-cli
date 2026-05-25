// SPDX-License-Identifier: GPL-3.0-only
#include "chat_agent.hpp"
#include "pu/renderer.hpp"

#include <iostream>
#include <sstream>

namespace pu::agents {

ChatAgent::ChatAgent(const std::string& name,
                     std::unique_ptr<pu::backend::Backend> backend,
                     const std::string& model_id)
    : name_(name), model_id_(model_id), backend_(std::move(backend)) {}

std::string ChatAgent::Handle(const std::string& input,
                              pu::agent::AgentContext& ctx) {
  // Inject system prompt if present and not already in history
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

  std::vector<pu::backend::Message> backend_history;
  for (const auto& cm : history_) {
    pu::backend::Message msg;
    if (cm.role == "user") {
      msg.role = pu::backend::Message::Role::kUser;
    } else if (cm.role == "chat" || cm.role == "assistant") {
      msg.role = pu::backend::Message::Role::kAssistant;
    } else if (cm.role == "system") {
      msg.role = pu::backend::Message::Role::kSystem;
    } else {
      continue;
    }
    msg.content = cm.content;
    backend_history.push_back(msg);
  }

  std::ostringstream full_response;
  std::error_code ec;
  auto renderer = pu::StreamingRenderer::Create(ctx.show_reasoning);
  backend_->Chat(backend_history,
                 [&](pu::backend::TokenType type, std::string_view token, bool is_final) {
                   if (type == pu::backend::TokenType::kContent) {
                     renderer(type, token, is_final);
                     if (!is_final) full_response << token;
                   } else if (type == pu::backend::TokenType::kReasoning && ctx.show_reasoning) {
                     renderer(type, token, is_final);
                   }
                 },
                 ec);
  if (ec) {
    std::string err = ec.message();
    std::cerr << "\nError: " << err << "\n";
    if (!history_.empty() && history_.back().role == "user") history_.pop_back();
    return "Error: " + err;
  }
  auto response = full_response.str();
  if (!response.empty()) history_.push_back({0, "", "chat", response, ""});
  return response;
}

void ChatAgent::ResetSession() { history_.clear(); }

std::vector<ChatMessage> ChatAgent::SaveState() const { return history_; }
void ChatAgent::LoadState(const std::vector<ChatMessage>& messages) {
  history_ = messages;
}

}  // namespace pu::agents
