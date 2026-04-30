// SPDX-License-Identifier: GPL-3.0-only

#include "chat_expert.hpp"
#include "pu/renderer.hpp"

#include <iostream>
#include <sstream>

namespace pu::experts {

ChatExpert::ChatExpert(const std::string& name,
                       std::unique_ptr<pu::backend::Backend> backend,
                       const std::string& model_id)
    : name_(name), model_id_(model_id), backend_(std::move(backend)) {}

std::string ChatExpert::Handle(const std::string& input,
                               pu::expert::ExpertContext& ctx) {
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
  try {
    auto renderer_cb = pu::StreamingRenderer::Create(ctx.show_reasoning);
    backend_->Chat(backend_history, [&](pu::backend::TokenType type,
                                       std::string_view token,
                                       bool is_final) {
      if (type == pu::backend::TokenType::kContent) {
        renderer_cb(type, token, is_final);
        if (!is_final) {
          full_response << token;
        }
      } else if (type == pu::backend::TokenType::kReasoning && ctx.show_reasoning) {
        renderer_cb(type, token, is_final);
      }
    });
  } catch (const std::exception& e) {
    std::cerr << "\nError: " << e.what() << "\n";
    if (!history_.empty() && history_.back().role == "user") {
      history_.pop_back();
    }
    return "Error: " + std::string(e.what());
  }

  std::string response = full_response.str();
  if (!response.empty()) {
    history_.push_back({0, "", "chat", response, ""});
  }
  return response;
}

void ChatExpert::ResetSession() {
  history_.clear();
}

std::vector<ChatMessage> ChatExpert::SaveState() const {
  return history_;
}

void ChatExpert::LoadState(const std::vector<ChatMessage>& messages) {
  history_ = messages;
}

}  // namespace pu::experts
