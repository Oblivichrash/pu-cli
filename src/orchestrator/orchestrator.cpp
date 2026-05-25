// SPDX-License-Identifier: GPL-3.0-only
#include "pu/orchestrator.hpp"

#include <sstream>

#include "pu/expert.hpp"

namespace pu {

Orchestrator::Orchestrator(std::shared_ptr<GlobalContext> ctx,
                           std::shared_ptr<CallStack> stack,
                           expert::ExpertManager& manager)
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

std::string Orchestrator::Process(const std::string& input) {
  if (stack_->IsEmpty()) {
    return manager_.Dispatch(input);
  }

  const StackFrame& top = stack_->Top();
  return manager_.CallExpert(top.agent_name, input);
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
