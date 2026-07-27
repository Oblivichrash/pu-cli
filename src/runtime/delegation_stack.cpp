// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/delegation_stack.hpp"
#include "pu/core/fork_merge_service.hpp"
#include "pu/agent_core.hpp"

#include "pu/error.hpp"
#include <stdexcept>

namespace pu::core {

std::shared_ptr<DelegationStack> DelegationStack::Create(
    std::shared_ptr<Context> root_context,
    agent::AgentManager& manager) {
  auto stack = std::make_shared<DelegationStack>(std::move(root_context));
  stack->Initialize(manager);
  return stack;
}

void DelegationStack::Initialize(agent::AgentManager& manager) {
  fork_service_ = std::make_shared<ForkMergeService>(
      manager, shared_from_this(), root_context_);
}

void DelegationStack::Push(const Delegation& delegation, std::shared_ptr<Context> context) {
  Frame frame;
  frame.delegation = delegation;
  frame.context = context ? context : std::make_shared<Context>("ctx-" + delegation.id);
  frames_.push_back(std::move(frame));
}

void DelegationStack::Push(const Delegation& delegation) {
  Push(delegation, nullptr);
}

SummaryReport DelegationStack::Pop() {
  if (frames_.empty()) {
    throw pu::Error("DelegationStack::Pop() on empty stack");
  }

  Frame frame = std::move(frames_.back());
  frames_.pop_back();

  if (!frame.delegation.result.has_value()) {
    SummaryReport report;
    report.status = SummaryReport::Status::kCompleted;
    report.summary = "[Delegation completed without explicit result]";
    frame.delegation.result = report;
  }

  return *frame.delegation.result;
}

DelegationStack::Frame& DelegationStack::Current() {
  if (frames_.empty()) {
    throw pu::Error("DelegationStack::Current() on empty stack");
  }
  return frames_.back();
}

const DelegationStack::Frame& DelegationStack::Current() const {
  if (frames_.empty()) {
    throw pu::Error("DelegationStack::Current() on empty stack");
  }
  return frames_.back();
}

Delegation& DelegationStack::CurrentDelegation() {
  return Current().delegation;
}

const Delegation& DelegationStack::CurrentDelegation() const {
  return Current().delegation;
}

std::shared_ptr<Context> DelegationStack::CurrentContext() {
  return Current().context;
}

void DelegationStack::Clear() {
  frames_.clear();
}

}  // namespace pu::core