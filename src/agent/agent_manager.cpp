// SPDX-License-Identifier: GPL-3.0-only
#include "pu/agent.hpp"

#include <iostream>

namespace pu::agent {

AgentManager::AgentManager() {}

void AgentManager::RegisterExpert(std::unique_ptr<BaseAgent> expert) {
  if (!expert) {
    return;
  }
  std::string name = expert->Name();
  if (experts_.count(name)) {
    std::cerr << "[AgentManager] Duplicate expert name: " << name << "\n";
    return;
  }
  experts_[name] = std::move(expert);
}

void AgentManager::SetActiveExpert(const std::string& name) {
  if (experts_.count(name)) {
    active_expert_ = name;
  }
}

std::string AgentManager::GetActiveExpert() const {
  return active_expert_;
}

void AgentManager::SetShowReasoning(bool enable) {
  show_reasoning_ = enable;
}

void AgentManager::SetRecentMessages(const std::vector<ChatMessage>& messages) {
  recent_messages_ = messages;
}

void AgentManager::SetProactiveEnabled(bool enabled) {
  proactive_enabled_ = enabled;
}

void AgentManager::SetProactiveThreshold(double threshold) {
  proactive_threshold_ = threshold;
  for (auto& [name, expert] : experts_) {
    expert->SetProactiveThreshold(threshold);
  }
}

void AgentManager::NotifyPanelMessage(const ChatMessage& msg) {
  for (auto& [name, expert] : experts_) {
    expert->OnPanelMessage(msg);
  }
}

std::vector<std::pair<std::string, std::string>> AgentManager::CollectProactiveReplies() {
  std::vector<std::pair<std::string, std::string>> replies;
  if (!proactive_enabled_) {
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

void AgentManager::SetConfirmationCallback(ConfirmationCallback cb) {
  confirmation_callback_ = std::move(cb);
}

void AgentManager::SetSystemPrompt(const std::string& expert_name,
                                   const std::string& prompt) {
  system_prompts_[expert_name] = prompt;
}

void AgentManager::SetGlobalContext(std::shared_ptr<GlobalContext> ctx) {
  global_ctx_ = std::move(ctx);
}

void AgentManager::SetCallStack(std::shared_ptr<CallStack> stack) {
  call_stack_ = std::move(stack);
}

AgentContext AgentManager::PrepareContext(const std::string& agent_name) {
  AgentContext ctx;
  ctx.call_expert = [this](const std::string& name, const std::string& inp) {
    return CallExpert(name, inp);
  };
  ctx.request_confirmation = confirmation_callback_ ? confirmation_callback_
      : [](const ConfirmationRequest& req) {
          std::cout << "[CONFIRM] " << req.description << " [y/N] ";
          std::string answer;
          std::getline(std::cin, answer);
          return (answer == "y" || answer == "Y") ? ConfirmationChoice::kApproveOnce
                                                  : ConfirmationChoice::kDeny;
        };
  ctx.working_dir = ".";
  ctx.show_reasoning = show_reasoning_;
  ctx.recent_panel_messages = recent_messages_;
  ctx.global_ctx = global_ctx_;
  ctx.call_stack = call_stack_;
  auto prompt_it = system_prompts_.find(agent_name);
  if (prompt_it != system_prompts_.end() && !prompt_it->second.empty()) {
    ctx.system_prompt = prompt_it->second;
  }
  return ctx;
}

BaseAgent* AgentManager::GetExpert(const std::string& name) const {
  auto it = experts_.find(name);
  if (it == experts_.end()) {
    return nullptr;
  }
  return it->second.get();
}

std::string AgentManager::ExecuteAgentWithContext(const std::string& agent_name,
                                                  const std::string& input,
                                                  AgentContext& ctx) {
  auto it = experts_.find(agent_name);
  if (it == experts_.end()) {
    return "Expert not found: " + agent_name;
  }
  return it->second->Handle(input, ctx);
}

std::string AgentManager::Dispatch(const std::string& input) {
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
    target = "chat";
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

  AgentContext ctx = PrepareContext(it->first);
  std::cout << "\n[" << it->first << "] " << std::flush;
  return it->second->Handle(message, ctx);
}

std::string AgentManager::CallExpert(const std::string& expert_name, const std::string& input) {
  auto it = experts_.find(expert_name);
  if (it == experts_.end()) {
    return "Expert not found: " + expert_name;
  }

  AgentContext ctx = PrepareContext(expert_name);
  return it->second->Handle(input, ctx);
}

void AgentManager::ClearSessions() {
  for (auto& [name, expert] : experts_) {
    expert->ResetSession();
  }
  active_expert_.clear();
}

std::unordered_map<std::string, std::vector<ChatMessage>> AgentManager::SnapshotExperts() const {
  std::unordered_map<std::string, std::vector<ChatMessage>> result;
  for (const auto& [name, expert] : experts_) {
    result[name] = expert->SaveState();
  }
  return result;
}

void AgentManager::RestoreExperts(
    const std::unordered_map<std::string, std::vector<ChatMessage>>& states) {
  for (const auto& [name, messages] : states) {
    auto it = experts_.find(name);
    if (it != experts_.end()) {
      it->second->LoadState(messages);
    }
  }
}

}  // namespace pu::agent
