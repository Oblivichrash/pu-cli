// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/fork_merge_service.hpp"
#include "pu/core/fact_extractor.hpp"
#include "pu/core/summary_generator.hpp"
#include "pu/agent/agent_manager.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <sstream>
#include <stdexcept>

namespace pu {

ForkMergeService::ForkMergeService(AgentManager& manager,
                                    std::shared_ptr<CallStack> delegation_stack,
                                    std::shared_ptr<Workspace> root_context)
    : manager_(manager),
      delegation_stack_(std::move(delegation_stack)),
      root_context_(std::move(root_context)) {}

ForkMergeService::ForkResult ForkMergeService::Fork(const std::string& agent_name,
                                                     const std::string& goal,
                                                     const std::string& branch_name) {
  ForkResult result;
  std::shared_ptr<Workspace> parent;
  if (delegation_stack_ && !delegation_stack_->IsEmpty()) {
    parent = delegation_stack_->CurrentContext();
  } else {
    parent = root_context_;
  }
  if (!parent) {
    result.message = "Error: Failed to fork context (no parent context)";
    return result;
  }
  auto child = parent->Fork(branch_name.empty() ? "" : branch_name);
  child->Append("system", "Fork created: " + goal);
  child->SetVar("fork_goal", goal);
  child->SetVar("fork_agent", agent_name);
  result.child_context = child;
  result.message = "Forked to branch: " + child->GetBranchName() + " (agent: " + agent_name + ")";
  return result;
}

ForkMergeService::MergeResult ForkMergeService::Merge(const std::string& message,
                                                       const std::string& strategy,
                                                       LLMProvider* provider) {
  MergeResult result;
  if (!delegation_stack_ || delegation_stack_->IsEmpty()) {
    result.report.status = HandoffReceipt::Status::kFailed;
    result.report.summary = "No active delegation to merge";
    result.message = "Error: No active delegation to merge";
    return result;
  }
  auto& frame = delegation_stack_->Current();
  auto child = frame.context;
  auto parent = child->GetParent();
  if (!parent) {
    result.report = PopDelegation(provider);
    result.message = "Popped delegation: " + result.report.summary;
    return result;
  }

  std::shared_ptr<Workspace> merge_ctx;
  result.report.status = HandoffReceipt::Status::kCompleted;

  if (strategy == "squash") {
    SummaryGenerator summary_gen(manager_);
    auto summary = summary_gen.Generate(child, frame.assignment, provider);
    merge_ctx = parent->Merge(child, message);
    // For squash, we just add summary messages instead of full history
    merge_ctx->Append("system", "[Squash Merge] " + message);
    merge_ctx->Append("system", "Summary: " + summary.summary);
    auto child_artifacts = child->GetArtifacts();
    for (const auto& a : child_artifacts) {
      merge_ctx->AddArtifact(a);
    }
    result.report.summary = "[Squash] " + summary.summary;
  } else {
    merge_ctx = parent->Merge(child, message);
    result.report.summary = message;
  }

  result.report.key_discoveries = child->GetArtifacts();
  result.merge_context = merge_ctx;
  delegation_stack_->Pop();

  if (!delegation_stack_->IsEmpty()) {
    delegation_stack_->Current().context = merge_ctx;
  }
  if (delegation_stack_->IsEmpty()) {
    root_context_ = merge_ctx;
  }

  result.message = "Merged: " + result.report.summary;
  return result;
}

void ForkMergeService::PrintTree(std::ostream& os) const {
  PrintTree(os, root_context_);
}

void ForkMergeService::PrintTree(std::ostream& os, const std::shared_ptr<Workspace>& root) const {
  if (!root) {
    os << "No root context available.\n";
    return;
  }

  os << "=== Fork Tree ===\n";
  std::function<void(const std::shared_ptr<Workspace>&, int, bool)> print_node;
  print_node = [&](const std::shared_ptr<Workspace>& ctx, int depth, bool is_last) {
    std::string icon;
    if (ctx == root) {
      icon = "\xf0\x9f\x8c\xbf";  // 🌿
    } else if (ctx->GetState() == Workspace::State::kMerged) {
      icon = "\xe2\x9c\x85";  // ✅
    } else if (ctx->GetState() == Workspace::State::kActive) {
      icon = "\xf0\x9f\x8c\xb1";  // 🌱
    } else {
      icon = "\xf0\x9f\x9a\xab";  // 🚫
    }

    std::string indent;
    for (int i = 0; i < depth; ++i) {
      indent += "   ";
    }
    if (depth > 0) {
      indent = indent.substr(0, indent.length() - 3) + (is_last ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ");
    }

    os << indent << icon << " " << ctx->GetBranchName()
       << " (" << ctx->HistorySize() << " msgs, ~" << ctx->GetTokenCount() << " tokens";
    if (ctx->GetState() == Workspace::State::kMerged) {
      os << ", merged";
    }
    os << ")\n";

    const auto& children = ctx->GetChildren();
    for (size_t i = 0; i < children.size(); ++i) {
      print_node(children[i], depth + 1, i == children.size() - 1);
    }
  };

  print_node(root, 0, true);
}

std::shared_ptr<Workspace> ForkMergeService::FindContext(const std::string& id_or_branch) const {
  auto ctx = root_context_;
  if (!ctx) return nullptr;
  std::shared_ptr<Workspace> found = nullptr;
  std::vector<std::shared_ptr<Workspace>> queue = {ctx};
  while (!queue.empty()) {
    auto cur = queue.back(); queue.pop_back();
    if (cur->GetId() == id_or_branch || cur->GetBranchName() == id_or_branch) {
      found = cur; break;
    }
    for (const auto& ch : cur->GetChildren()) queue.push_back(ch);
  }
  return found;
}

size_t ForkMergeService::PruneMerged() {
  if (!root_context_) return 0;

  size_t total_removed = 0;
  std::function<void(std::shared_ptr<Workspace>&)> prune_recursive;
  prune_recursive = [&](std::shared_ptr<Workspace>& ctx) {
    if (!ctx) return;
    total_removed += ctx->RemoveMergedChildren();
    auto children = ctx->GetChildren();
    for (auto& child : children) {
      auto mutable_child = std::const_pointer_cast<Workspace>(child);
      prune_recursive(mutable_child);
    }
  };

  prune_recursive(root_context_);
  return total_removed;
}

std::vector<Artifact> ForkMergeService::ExtractFacts(const std::shared_ptr<Workspace>& ctx,
                                                      const std::string& goal) {
  FactExtractor extractor;
  return extractor.Extract(ctx, goal);
}

HandoffReceipt ForkMergeService::GenerateSummary(const std::shared_ptr<Workspace>& child_ctx,
                                                  const Assignment& delegation,
                                                  LLMProvider* provider) {
  SummaryGenerator summary_gen(manager_);
  return summary_gen.Generate(child_ctx, delegation, provider);
}

void ForkMergeService::InjectSummaryIntoParent(const HandoffReceipt& report) {
  if (delegation_stack_ && !delegation_stack_->IsEmpty()) {
    auto parent_ctx = delegation_stack_->CurrentContext();
    if (parent_ctx) {
      parent_ctx->Append("system", "[Sub-task] " + report.summary);
      for (const auto& f : report.key_discoveries) {
        parent_ctx->AddArtifact(f);
      }
    }
  } else if (root_context_) {
    root_context_->Append("system", "[Completed delegation] " + report.summary);
  }
}

HandoffReceipt ForkMergeService::PopDelegation(LLMProvider* provider) {
  if (!delegation_stack_ || delegation_stack_->IsEmpty()) {
    HandoffReceipt report;
    report.status = HandoffReceipt::Status::kFailed;
    report.summary = "No active delegation to pop";
    return report;
  }
  auto& frame = delegation_stack_->Current();
  auto report = GenerateSummary(frame.context, frame.assignment, provider);
  frame.assignment.result = report;
  delegation_stack_->Pop();
  return report;
}

}  // namespace pu
