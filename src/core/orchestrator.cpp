// SPDX-License-Identifier: GPL-3.0-only
#include "pu/orchestrator.hpp"

#include <iostream>
#include <sstream>

#include "pu/agent.hpp"
#include "pu/context.hpp"
#include "pu/stack.hpp"

namespace pu {

Orchestrator::Orchestrator(std::shared_ptr<GlobalContext> ctx,
                           std::shared_ptr<CallStack> stack,
                           agent::AgentManager& manager)
    : ctx_(std::move(ctx)), stack_(std::move(stack)), manager_(manager) {}

bool Orchestrator::HandleCommand(const std::string& input, std::string& output) {
  if (input.rfind("/push ", 0) == 0) {
    std::string agent_name = input.substr(6);
    size_t start = agent_name.find_first_not_of(" \t");
    if (start != std::string::npos) {
      agent_name = agent_name.substr(start);
    } else {
      output = "Error: agent name required";
      return true;
    }
    size_t end = agent_name.find_last_not_of(" \t");
    if (end != std::string::npos) {
      agent_name = agent_name.substr(0, end + 1);
    }
    if (agent_name.empty()) {
      output = "Error: agent name required";
      return true;
    }
    Push(agent_name);
    output = "Pushed agent: " + agent_name;
    return true;
  }

  if (input == "/pop") {
    if (stack_->IsEmpty()) {
      output = "Error: stack is empty";
      return true;
    }
    Pop();
    output = "Popped agent";
    return true;
  }

  if (input == "/stack") {
    output = ShowStack();
    return true;
  }

  return false;
}

std::string Orchestrator::Process(const std::string& input, bool show_reasoning) {
  std::string current_input = input;
  std::string final_response;

  while (true) {
    if (stack_->IsEmpty()) {
      final_response = manager_.Dispatch(current_input);
      break;
    }

    const StackFrame& top = stack_->Top();
    std::string agent_name = top.agent_name;

    // Build context for stack-based calls
    agent::AgentContext ctx;
    auto* mgr = &manager_;
    ctx.call_expert = [mgr](const std::string& name, const std::string& inp) {
      return mgr->CallAgent(name, inp);
    };
    ctx.request_confirmation = [](const agent::ConfirmationRequest& req) {
      std::cout << "[CONFIRM] " << req.description << " [y/N] ";
      std::string answer;
      std::getline(std::cin, answer);
      return (answer == "y" || answer == "Y") ? agent::ConfirmationChoice::kApproveOnce
                                              : agent::ConfirmationChoice::kDeny;
    };
    ctx.working_dir = ".";
    ctx.show_reasoning = show_reasoning;
    // We don't set global_ctx or call_stack here; they are not needed in stack context
    // (the agent will use the ones set in AgentManager if needed)

    std::string response = manager_.ExecuteAgentWithContext(agent_name, current_input, ctx);

    if (ctx.pending_action.type == agent::PendingAction::Type::kPush) {
      Push(ctx.pending_action.agent_name);
      current_input = "";
      continue;
    }

    if (ctx.pending_action.type == agent::PendingAction::Type::kPop) {
      if (!stack_->IsEmpty()) {
        Pop();
      }
      if (stack_->IsEmpty()) {
        final_response = response;
        break;
      } else {
        current_input = response;
        continue;
      }
    }

    final_response = response;
    break;
  }

  return final_response;
}

void Orchestrator::Push(const std::string& agent_name) {
  StackFrame frame;
  frame.agent_name = agent_name;
  frame.invocation_id = "manual_" + std::to_string(stack_->Size() + 1);
  stack_->Push(frame);
}

void Orchestrator::Pop() {
  if (!stack_->IsEmpty()) {
    stack_->Pop();
  }
}

std::string Orchestrator::ShowStack() const {
  if (stack_->IsEmpty()) {
    return "Stack is empty";
  }
  std::ostringstream oss;
  oss << "Stack (top to bottom):\n";
  const auto& frames = stack_->GetFrames();
  for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
    oss << "  " << it->agent_name << " (" << it->invocation_id << ")\n";
  }
  return oss.str();
}

}  // namespace pu
