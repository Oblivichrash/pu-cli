// SPDX-License-Identifier: GPL-3.0-only
#include "pu/agent_core.hpp"

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

std::vector<std::string> AgentManager::GetAgentNames() const {
  std::vector<std::string> names;
  names.reserve(agents_.size());
  for (const auto& [name, agent] : agents_) {
    names.push_back(name);
  }
  return names;
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

std::optional<std::string> AgentManager::GetSystemPrompt(const std::string& agent_name) const {
  auto it = system_prompts_.find(agent_name);
  if (it != system_prompts_.end() && !it->second.empty()) {
    return it->second;
  }
  return std::nullopt;
}

void AgentManager::SetConfirmationCallback(ConfirmationCallback cb) {
  confirmation_callback_ = std::move(cb);
}

void AgentManager::SetShowReasoning(bool enable) {
  show_reasoning_ = enable;
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
