// SPDX-License-Identifier: GPL-3.0-only
#include "pu/executor.hpp"

#include <iostream>

namespace pu::agent {

AgentExecutor::AgentExecutor(AgentManager& manager) : manager_(manager) {}

AgentContext AgentExecutor::PrepareContext(const std::string& agent_name) {
  AgentContext ctx;
  ctx.call_expert = [this](const std::string& name, const std::string& inp) {
    return CallAgent(name, inp);
  };
  ctx.request_confirmation = manager_.GetConfirmationCallback() ? manager_.GetConfirmationCallback()
      : [](const ConfirmationRequest& req) {
          std::cout << "[CONFIRM] " << req.description << " [y/N] ";
          std::string answer;
          std::getline(std::cin, answer);
          return (answer == "y" || answer == "Y") ? ConfirmationChoice::kApproveOnce
                                                  : ConfirmationChoice::kDeny;
        };
  ctx.working_dir = ".";
  ctx.show_reasoning = manager_.GetShowReasoning();
  ctx.global_ctx = manager_.GetGlobalContext();
  ctx.call_stack = manager_.GetCallStack();

  auto prompt = manager_.GetSystemPrompt(agent_name);
  if (prompt) {
    ctx.system_prompt = *prompt;
  }
  return ctx;
}

std::string AgentExecutor::Execute(const std::string& agent_name, const std::string& input, AgentContext& ctx) {
  auto* agent = manager_.GetAgent(agent_name);
  if (!agent) {
    return "Agent not found: " + agent_name;
  }
  return agent->Handle(input, ctx);
}

std::string AgentExecutor::CallAgent(const std::string& agent_name, const std::string& input) {
  AgentContext ctx = PrepareContext(agent_name);
  return Execute(agent_name, input, ctx);
}

std::string AgentExecutor::Dispatch(const std::string& input) {
  std::string target;
  std::string message = input;

  if (!input.empty() && input[0] == '@') {
    size_t space_pos = input.find(' ');
    if (space_pos != std::string::npos) {
      target = input.substr(1, space_pos - 1);
      message = input.substr(space_pos + 1);
    } else {
      return "";
    }
  }

  if (target.empty()) {
    target = manager_.GetActiveAgent();
  }

  if (target.empty() || !manager_.GetAgent(target)) {
    if (manager_.GetAgent("chat")) {
      target = "chat";
    } else {
      auto names = manager_.GetAgentNames();
      if (!names.empty()) {
        target = names[0];
      } else {
        return "No agents available.";
      }
    }
  }

  if (manager_.GetActiveAgent().empty() && (!input.empty() && input[0] != '@')) {
    manager_.SetActiveAgent(target);
  }

  AgentContext ctx = PrepareContext(target);
  std::cout << "\n[" << target << "] " << std::flush;
  return Execute(target, message, ctx);
}

}  // namespace pu::agent
