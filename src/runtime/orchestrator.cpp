// SPDX-License-Identifier: GPL-3.0-only
#include "pu/orchestrator.hpp"
#include "pu/core/command_handler.hpp"
#include "pu/executor.hpp"
#include <algorithm>
#include <chrono>
#include <functional>
#include <sstream>
#include <stdexcept>

namespace pu {

Orchestrator::Orchestrator(agent::AgentManager& manager)
    : manager_(manager) {}

void Orchestrator::SetDelegationStack(std::shared_ptr<CallStack> stack) {
  delegation_stack_ = std::move(stack);
  if (delegation_stack_) {
    root_context_ = delegation_stack_->GetRootContext();
    command_handler_ = std::make_shared<CommandHandler>(
        manager_,
        delegation_stack_->GetForkMergeService(),
        delegation_stack_,
        root_context_);
  }
}

std::vector<Artifact> Orchestrator::ExtractFacts(
    const std::shared_ptr<Workspace>& ctx,
    const std::string& goal) {
  auto fms = delegation_stack_ ? delegation_stack_->GetForkMergeService() : nullptr;
  if (fms) {
    return fms->ExtractFacts(ctx, goal);
  }
  return {};
}

HandoffReceipt Orchestrator::GenerateSummary(
    const std::shared_ptr<Workspace>& child_ctx,
    const Assignment& delegation) {
  auto fms = delegation_stack_ ? delegation_stack_->GetForkMergeService() : nullptr;
  if (fms) {
    return fms->GenerateSummary(child_ctx, delegation);
  }
  HandoffReceipt report;
  report.status = HandoffReceipt::Status::kFailed;
  report.summary = "ForkMergeService not initialized";
  return report;
}

void Orchestrator::InjectSummaryIntoParent(const HandoffReceipt& report) {
  auto fms = delegation_stack_ ? delegation_stack_->GetForkMergeService() : nullptr;
  if (fms) {
    fms->InjectSummaryIntoParent(report);
  }
}

std::shared_ptr<Workspace> Orchestrator::ForkContext(
    const std::string& agent_name,
    const std::string& goal,
    const std::string& branch_name) {
  auto fms = delegation_stack_ ? delegation_stack_->GetForkMergeService() : nullptr;
  if (fms) {
    auto result = fms->Fork(agent_name, goal, branch_name);
    return result.child_context;
  }
  return nullptr;
}

void Orchestrator::PrintForkTree(std::ostream& os) {
  auto fms = delegation_stack_ ? delegation_stack_->GetForkMergeService() : nullptr;
  if (fms) {
    fms->PrintTree(os);
  } else {
    os << "No fork service available.\n";
  }
}

size_t Orchestrator::PruneMergedForks() {
  auto fms = delegation_stack_ ? delegation_stack_->GetForkMergeService() : nullptr;
  if (fms) {
    return fms->PruneMerged();
  }
  return 0;
}

HandoffReceipt Orchestrator::MergeContext(
    const std::string& message,
    const std::string& strategy) {
  auto fms = delegation_stack_ ? delegation_stack_->GetForkMergeService() : nullptr;
  if (fms) {
    auto result = fms->Merge(message, strategy);
    return result.report;
  }
  HandoffReceipt report;
  report.status = HandoffReceipt::Status::kFailed;
  report.summary = "ForkMergeService not initialized";
  return report;
}

bool Orchestrator::HandleCommand(const std::string& input, std::string& output) {
  if (command_handler_ && command_handler_->Handle(input, output)) {
    return true;
  }
  return false;
}

std::string Orchestrator::Process(const std::string& input) {
  std::string current_input = input;
  std::string final_response;
  agent::AgentExecutor executor(manager_);
  if (root_context_) executor.SetRootContext(root_context_);
  if (delegation_stack_) executor.SetDelegationStack(delegation_stack_);
  if (delegation_stack_ && !delegation_stack_->IsEmpty()) {
    const auto& frame = delegation_stack_->Current();
    std::string agent_name = frame.assignment.agent_name;
    auto child_ctx = frame.context;
    agent::AgentContext ctx = executor.PrepareContext(agent_name, child_ctx);
    std::string response = executor.Execute(agent_name, current_input, ctx);
    if (ctx.pending_action.type == agent::PendingAction::Type::kPop) {
      if (!delegation_stack_->IsEmpty()) {
        auto child = delegation_stack_->CurrentContext();
        auto parent = child ? child->GetParent() : nullptr;
        if (parent) {
          auto report = MergeContext("Task completed", "merge");
          InjectSummaryIntoParent(report);
        } else {
          auto report = PopDelegation();
          InjectSummaryIntoParent(report);
        }
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
    final_response = executor.Dispatch(current_input);
  }
  return final_response;
}

bool Orchestrator::PushDelegation(
    const std::string& agent_name, const std::string& goal) {
  if (!delegation_stack_) return false;
  Assignment deleg;
  deleg.goal = goal;
  deleg.agent_name = agent_name;
  deleg.depth = static_cast<int>(delegation_stack_->Depth());
  deleg.id = Assignment::GenerateId();
  deleg.created_at = std::chrono::steady_clock::now();
  deleg.deadline = deleg.created_at + std::chrono::seconds(30);
  delegation_stack_->Push(deleg);
  return true;
}

HandoffReceipt Orchestrator::PopDelegation() {
  auto fms = delegation_stack_ ? delegation_stack_->GetForkMergeService() : nullptr;
  if (fms) {
    return fms->PopDelegation();
  }
  if (!delegation_stack_ || delegation_stack_->IsEmpty()) {
    HandoffReceipt report;
    report.status = HandoffReceipt::Status::kFailed;
    report.summary = "No active delegation to pop";
    return report;
  }
  auto& frame = delegation_stack_->Current();
  auto report = GenerateSummary(frame.context, frame.assignment);
  frame.assignment.result = report;
  delegation_stack_->Pop();
  return report;
}

}  // namespace pu
