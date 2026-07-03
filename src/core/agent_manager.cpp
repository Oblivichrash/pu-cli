// SPDX-License-Identifier: GPL-3.0-only
#include "pu/agent.hpp"

#include <iostream>

namespace pu::agent {

AgentManager::AgentManager() {}

void AgentManager::RegisterAgent(std::unique_ptr<BaseAgent> agent) {
  if (!agent) {
    return;
  }
  std::string name = agent->Name();
  if (agents_.count(name)) {
    std::cerr << "[AgentManager] Duplicate agent name: " << name << "\n";
    return;
  }
  agents_[name] = std::move(agent);
}

void AgentManager::SetActiveAgent(const std::string& name) {
  if (agents_.count(name)) {
    active_agent_ = name;
  }
}

std::string AgentManager::GetActiveAgent() const {
  return active_agent_;
}

BaseAgent* AgentManager::GetAgent(const std::string& name) const {
  auto it = agents_.find(name);
  if (it == agents_.end()) return nullptr;
  return it->second.get();
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
  for (auto& [name, agent] : agents_) {
    agent->SetProactiveThreshold(threshold);
  }
}

void AgentManager::NotifyPanelMessage(const ChatMessage& msg) {
  for (auto& [name, agent] : agents_) {
    agent->OnPanelMessage(msg);
  }
}

std::vector<std::pair<std::string, std::string>> AgentManager::CollectProactiveReplies() {
  std::vector<std::pair<std::string, std::string>> replies;
  if (!proactive_enabled_) {
    return replies;
  }
  for (auto& [name, agent] : agents_) {
    auto reply = agent->ProactiveReply();
    if (reply) {
      replies.emplace_back(name, *reply);
    }
  }
  return replies;
}

void AgentManager::SetConfirmationCallback(ConfirmationCallback cb) {
  confirmation_callback_ = std::move(cb);
}

void AgentManager::SetSystemPrompt(const std::string& agent_name,
                                   const std::string& prompt) {
  system_prompts_[agent_name] = prompt;
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
    return CallAgent(name, inp);
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

std::string AgentManager::ExecuteAgentWithContext(const std::string& agent_name,
                                                  const std::string& input,
                                                  AgentContext& ctx) {
  auto it = agents_.find(agent_name);
  if (it == agents_.end()) {
    return "Agent not found: " + agent_name;
  }
  return it->second->Handle(input, ctx);
}

std::string AgentManager::Dispatch(const std::string& input) {
  if (agents_.empty()) {
    return "No agents available.";
  }

  std::string target = active_agent_;
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

  auto it = agents_.find(target);
  if (it == agents_.end()) {
    it = agents_.find("chat");
    if (it == agents_.end()) {
      it = agents_.begin();
    }
  }

  if (active_agent_.empty() && input[0] != '@') {
    active_agent_ = it->first;
  }

  AgentContext ctx = PrepareContext(it->first);
  std::cout << "\n[" << it->first << "] " << std::flush;
  return it->second->Handle(message, ctx);
}

std::string AgentManager::CallAgent(const std::string& agent_name, const std::string& input) {
  auto it = agents_.find(agent_name);
  if (it == agents_.end()) {
    return "Agent not found: " + agent_name;
  }

  AgentContext ctx = PrepareContext(agent_name);
  return it->second->Handle(input, ctx);
}

void AgentManager::ClearSessions() {
  for (auto& [name, agent] : agents_) {
    agent->ResetSession();
  }
  active_agent_.clear();
}

std::unordered_map<std::string, std::vector<ChatMessage>> AgentManager::SnapshotAgents() const {
  std::unordered_map<std::string, std::vector<ChatMessage>> result;
  for (const auto& [name, agent] : agents_) {
    result[name] = agent->SaveState();
  }
  return result;
}

void AgentManager::RestoreAgents(
    const std::unordered_map<std::string, std::vector<ChatMessage>>& states) {
  for (const auto& [name, messages] : states) {
    auto it = agents_.find(name);
    if (it != agents_.end()) {
      it->second->LoadState(messages);
    }
  }
}

}  // namespace pu::agent
