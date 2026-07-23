// SPDX-License-Identifier: GPL-3.0-only
#include "pu/executor.hpp"

#include <iostream>

namespace pu::agent {

AgentExecutor::AgentExecutor(AgentManager& manager) : manager_(manager) {}

void AgentExecutor::SetRootContext(std::shared_ptr<core::Context> root_context) {
  root_context_ = std::move(root_context);
  if (root_context_) {
    stack_ = std::make_shared<core::DelegationStack>(root_context_);
  }
}

AgentContext AgentExecutor::PrepareContext(const std::string& agent_name) {
  AgentContext ctx;

  if (!root_context_) {
    root_context_ = std::make_shared<core::Context>("root");
    stack_ = std::make_shared<core::DelegationStack>(root_context_);
  }

  ctx.context = root_context_;

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

  auto prompt = manager_.GetSystemPrompt(agent_name);
  if (prompt) {
    ctx.system_prompt = *prompt;
  }
  return ctx;
}

AgentContext AgentExecutor::PrepareContext(const std::string& agent_name,
                                           std::shared_ptr<core::Context> external_ctx) {
  AgentContext ctx;

  if (external_ctx) {
    ctx.context = external_ctx;
  } else {
    if (!root_context_) {
      root_context_ = std::make_shared<core::Context>("root");
      stack_ = std::make_shared<core::DelegationStack>(root_context_);
    }
    ctx.context = root_context_;
  }

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

  auto prompt = manager_.GetSystemPrompt(agent_name);
  if (prompt) {
    ctx.system_prompt = *prompt;
  }

  if (external_ctx) {
    auto history = external_ctx->GetHistory();
    if (history.size() > 20) {
      ctx.recent_panel_messages = std::vector<ChatMessage>(history.end() - 20, history.end());
    } else {
      ctx.recent_panel_messages = history;
    }
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

  if (root_context_) {
    root_context_->Append("user", message);
  }

  AgentContext ctx = PrepareContext(target);
  std::cout << "\n[" << target << "] " << std::flush;
  std::string response = Execute(target, message, ctx);

  if (root_context_ && !response.empty()) {
    root_context_->Append(target, response);
  }

  return response;
}

}  // namespace pu::agent
