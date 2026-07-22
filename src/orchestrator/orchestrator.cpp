// SPDX-License-Identifier: GPL-3.0-only
#include "pu/orchestrator.hpp"
#include "pu/executor.hpp"

#include <sstream>

namespace pu {

Orchestrator::Orchestrator(std::shared_ptr<GlobalContext> ctx,
                           std::shared_ptr<CallStack> stack,
                           agent::AgentManager& manager)
    : ctx_(std::move(ctx)), stack_(std::move(stack)), manager_(manager) {}

void Orchestrator::SetDelegationStack(std::shared_ptr<core::DelegationStack> stack) {
  delegation_stack_ = std::move(stack);
  if (delegation_stack_) {
    root_context_ = delegation_stack_->GetRootContext();
  }
}

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

std::string Orchestrator::Process(const std::string& input) {
  std::string current_input = input;
  std::string final_response;
  agent::AgentExecutor executor(manager_);

  if (root_context_) {
    executor.SetRootContext(root_context_);
  }

  if (delegation_stack_ && !delegation_stack_->IsEmpty()) {
    const auto& frame = delegation_stack_->Current();
    std::string agent_name = frame.delegation.agent_name;

    agent::AgentContext ctx = executor.PrepareContext(agent_name);
    std::string response = executor.Execute(agent_name, current_input, ctx);

    if (ctx.pending_action.type == agent::PendingAction::Type::kPush) {
      Push(ctx.pending_action.agent_name);
      current_input = "";
      return Process(current_input);
    }

    if (ctx.pending_action.type == agent::PendingAction::Type::kPop) {
      if (!delegation_stack_->IsEmpty()) {
        PopDelegation();
      }
      if (delegation_stack_->IsEmpty()) {
        final_response = response;
      } else {
        current_input = response;
        return Process(current_input);
      }
    }

    final_response = response;
  } else {
    // Fallback to legacy dispatch
    while (true) {
      if (stack_->IsEmpty()) {
        final_response = executor.Dispatch(current_input);
        break;
      }

      const StackFrame& top = stack_->Top();
      std::string agent_name = top.agent_name;

      agent::AgentContext ctx = executor.PrepareContext(agent_name);
      std::string response = executor.Execute(agent_name, current_input, ctx);

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

bool Orchestrator::PushDelegation(const std::string& agent_name, const std::string& goal) {
  if (!delegation_stack_) return false;

  core::Delegation deleg(goal, agent_name, {}, static_cast<int>(delegation_stack_->Depth()));
  deleg.id = core::Delegation::GenerateId();
  deleg.created_at = std::chrono::steady_clock::now();

  deleg.deadline = deleg.created_at + std::chrono::seconds(30);

  delegation_stack_->Push(deleg);
  return true;
}

core::SummaryReport Orchestrator::PopDelegation() {
  if (!delegation_stack_ || delegation_stack_->IsEmpty()) {
    core::SummaryReport report;
    report.status = core::SummaryReport::Status::kFailed;
    report.summary = "No active delegation to pop";
    return report;
  }
  return delegation_stack_->Pop();
}

}  // namespace pu
