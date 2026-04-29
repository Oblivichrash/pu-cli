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
  history_.push_back({pu::backend::Message::Role::kUser, input});

  std::ostringstream full_response;
  try {
    auto renderer_cb = pu::StreamingRenderer::Create(ctx.show_reasoning);
    backend_->Chat(history_, [&](pu::backend::TokenType type,
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
    if (!history_.empty() && history_.back().role == pu::backend::Message::Role::kUser) {
      history_.pop_back();
    }
    return "Error: " + std::string(e.what());
  }

  std::string response = full_response.str();
  if (!response.empty()) {
    history_.push_back({pu::backend::Message::Role::kAssistant, response});
  }
  return response;
}

void ChatExpert::ResetSession() {
  history_.clear();
}

std::vector<ChatMessage> ChatExpert::SaveState() const {
  std::vector<ChatMessage> messages;
  for (const auto& msg : history_) {
    std::string role;
    switch (msg.role) {
      case pu::backend::Message::Role::kUser: role = "user"; break;
      case pu::backend::Message::Role::kAssistant: role = "chat"; break;
      case pu::backend::Message::Role::kSystem: role = "system"; break;
      default: role = "unknown"; break;
    }
    messages.push_back({/*id=*/0, /*timestamp=*/"", role, msg.content});
  }
  return messages;
}

void ChatExpert::LoadState(const std::vector<ChatMessage>& messages) {
  history_.clear();
  for (const auto& msg : messages) {
    pu::backend::Message::Role role;
    if (msg.role == "user") {
      role = pu::backend::Message::Role::kUser;
    } else if (msg.role == "chat" || msg.role == "assistant") {
      role = pu::backend::Message::Role::kAssistant;
    } else if (msg.role == "system") {
      role = pu::backend::Message::Role::kSystem;
    } else {
      continue;  // ignore unknown roles
    }
    history_.push_back({role, msg.content});
  }
}

}  // namespace pu::experts
