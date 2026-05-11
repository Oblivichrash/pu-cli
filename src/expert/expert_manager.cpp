// SPDX-License-Identifier: GPL-3.0-only

#include "pu/expert.hpp"
#include <iostream>

namespace pu::expert {

ExpertManager::ExpertManager()
    : proactive_engine_(std::make_unique<ProactiveEngine>()) {}

void ExpertManager::RegisterExpert(std::unique_ptr<BaseExpert> expert) {
  if (!expert) {
    return;
  }
  std::string name = expert->Name();
  if (experts_.count(name)) {
    std::cerr << "[ExpertManager] Duplicate expert name: " << name << "\n";
    return;
  }
  experts_[name] = std::move(expert);
}

void ExpertManager::SetActiveExpert(const std::string& name) {
  if (experts_.count(name)) {
    active_expert_ = name;
  }
}

std::string ExpertManager::GetActiveExpert() const {
  return active_expert_;
}

void ExpertManager::SetShowReasoning(bool enable) {
  show_reasoning_ = enable;
}

void ExpertManager::SetRecentMessages(const std::vector<ChatMessage>& messages) {
  proactive_engine_->SetRecentMessages(messages);
}

void ExpertManager::SetProactiveEnabled(bool enabled) {
  proactive_engine_->SetEnabled(enabled);
}

void ExpertManager::SetProactiveThreshold(double threshold) {
  proactive_engine_->SetThreshold(threshold);
  for (auto& [name, expert] : experts_) {
    expert->SetProactiveThreshold(threshold);
  }
}

void ExpertManager::NotifyPanelMessage(const ChatMessage& msg) {
  for (auto& [name, expert] : experts_) {
    expert->OnPanelMessage(msg);
  }
}

std::vector<std::pair<std::string, std::string>> ExpertManager::CollectProactiveReplies() {
  std::vector<std::pair<std::string, std::string>> replies;
  if (!proactive_engine_->IsEnabled()) {
    return replies;
  }
  for (auto& [name, expert] : experts_) {
    auto reply = expert->ProactiveReply();
    if (reply) {
      replies.emplace_back(name, *reply);
    }
  }
  return replies;
}

std::string ExpertManager::Dispatch(const std::string& input) {
  if (experts_.empty()) {
    return "No experts available.";
  }

  std::string target = active_expert_;
  std::string message = input;

  if (!input.empty() && input[0] == '@') {
    size_t space_pos = input.find(' ');
    if (space_pos != std::string::npos) {
      target = input.substr(1, space_pos - 1);
      message = input.substr(space_pos + 1);
    } else {
      return "";
    }
  } else if (target.empty()) {
    target = experts_.count("chat") ? "chat" : experts_.begin()->first;
  }

  auto it = experts_.find(target);
  if (it == experts_.end()) {
    it = experts_.find("chat");
    if (it == experts_.end()) {
      it = experts_.begin();
    }
  }

  if (active_expert_.empty() && input[0] != '@') {
    active_expert_ = it->first;
  }

  ExpertContext ctx;
  ctx.call_expert = [this](const std::string& name, const std::string& inp) {
    return CallExpert(name, inp);
  };
  ctx.request_confirmation = [](const std::string& prompt) {
    std::cout << "[CONFIRM] " << prompt << " [y/N] ";
    std::string answer;
    std::getline(std::cin, answer);
    return (answer == "y" || answer == "Y");
  };
  ctx.working_dir = ".";
  ctx.show_reasoning = show_reasoning_;
  ctx.recent_panel_messages = proactive_engine_->GetRecentMessages();

  std::cout << "\n[" << it->first << "] " << std::flush;
  return it->second->Handle(message, ctx);
}

std::string ExpertManager::CallExpert(const std::string& expert_name, const std::string& input) {
  auto it = experts_.find(expert_name);
  if (it == experts_.end()) {
    return "Expert not found: " + expert_name;
  }

  ExpertContext ctx;
  ctx.call_expert = [this](const std::string& name, const std::string& inp) {
    return CallExpert(name, inp);
  };
  ctx.working_dir = ".";
  return it->second->Handle(input, ctx);
}

void ExpertManager::ClearSessions() {
  for (auto& [name, expert] : experts_) {
    expert->ResetSession();
  }
  active_expert_.clear();
}

std::unordered_map<std::string, std::vector<ChatMessage>> ExpertManager::SnapshotExperts() const {
  std::unordered_map<std::string, std::vector<ChatMessage>> result;
  for (const auto& [name, expert] : experts_) {
    result[name] = expert->SaveState();
  }
  return result;
}

void ExpertManager::RestoreExperts(
    const std::unordered_map<std::string, std::vector<ChatMessage>>& states) {
  for (const auto& [name, messages] : states) {
    auto it = experts_.find(name);
    if (it != experts_.end()) {
      it->second->LoadState(messages);
    }
  }
}

}  // namespace pu::expert
