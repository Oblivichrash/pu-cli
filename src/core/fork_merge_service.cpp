// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/fork_merge_service.hpp"
#include "pu/core/artifact_extractor.hpp"
#include "pu/core/summary_generator.hpp"
#include "pu/agent/agent_manager.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <sstream>
#include <stdexcept>

namespace pu {

ForkMergeService::ForkMergeService(AgentManager& manager,
                                    std::shared_ptr<Workspace> root_workspace)
    : manager_(manager),
      root_workspace_(std::move(root_workspace)) {}

ForkMergeService::ForkResult ForkMergeService::Fork(const std::shared_ptr<Workspace>& parent,
                                                     const std::string& agent_name,
                                                     const std::string& goal,
                                                     const std::string& branch_name) {
  ForkResult result;
  if (!parent) {
    result.message = "Error: Failed to fork workspace (no parent workspace)";
    return result;
  }
  auto child = parent->Fork(branch_name.empty() ? "" : branch_name);
  child->Append("system", "Fork created: " + goal);
  child->SetVar("fork_goal", goal);
  child->SetVar("fork_agent", agent_name);
  result.child_workspace = child;
  result.message = "Forked to branch: " + child->GetBranchName() + " (agent: " + agent_name + ")";
  return result;
}

ForkMergeService::MergeResult ForkMergeService::Merge(const std::shared_ptr<Workspace>& child,
                                                       const std::string& message,
                                                       const std::string& strategy,
                                                       LLMProvider* provider) {
  MergeResult result;
  if (!child) {
    result.report.status = HandoffReceipt::Status::kFailed;
    result.report.summary = "No active delegation to merge";
    result.message = "Error: No active delegation to merge";
    return result;
  }
  auto parent = child->GetParent();
  if (!parent) {
    // Root workspace has no parent: generate a final summary.
    SummaryGenerator summary_gen(manager_);
    Assignment dummy;
    dummy.goal = "merge";
    dummy.agent_name = "";
    auto summary = summary_gen.Generate(child, dummy, provider);
    result.report = summary;
    result.message = "Popped delegation: " + result.report.summary;
    return result;
  }

  std::shared_ptr<Workspace> merge_workspace;
  result.report.status = HandoffReceipt::Status::kCompleted;

  if (strategy == "squash") {
    SummaryGenerator summary_gen(manager_);
    Assignment dummy;
    dummy.goal = "merge";
    dummy.agent_name = "";
    auto summary = summary_gen.Generate(child, dummy, provider);
    merge_workspace = parent->Merge(child, message);
    merge_workspace->Append("system", "[Squash Merge] " + message);
    merge_workspace->Append("system", "Summary: " + summary.summary);
    auto child_artifacts = child->GetArtifacts();
    for (const auto& a : child_artifacts) {
      merge_workspace->AddArtifact(a);
    }
    result.report.summary = "[Squash] " + summary.summary;
  } else {
    merge_workspace = parent->Merge(child, message);
    result.report.summary = message;
  }

  result.report.key_discoveries = child->GetArtifacts();
  result.merge_workspace = merge_workspace;
  result.message = "Merged: " + result.report.summary;
  return result;
}

void ForkMergeService::PrintTree(std::ostream& os) const {
  PrintTree(os, root_workspace_);
}

void ForkMergeService::PrintTree(std::ostream& os, const std::shared_ptr<Workspace>& root) const {
  if (!root) {
    os << "No root workspace available.\n";
    return;
  }

  os << "=== Fork Tree ===\n";
  std::function<void(const std::shared_ptr<Workspace>&, int, bool)> print_node;
  print_node = [&](const std::shared_ptr<Workspace>& ws, int depth, bool is_last) {
    std::string icon;
    if (ws == root) {
      icon = "\xf0\x9f\x8c\xbf";  // 🌿
    } else if (ws->GetState() == Workspace::State::kMerged) {
      icon = "\xe2\x9c\x85";  // ✅
    } else if (ws->GetState() == Workspace::State::kActive) {
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

    os << indent << icon << " " << ws->GetBranchName()
       << " (" << ws->HistorySize() << " msgs, ~" << ws->GetTokenCount() << " tokens";
    if (ws->GetState() == Workspace::State::kMerged) {
      os << ", merged";
    }
    os << ")\n";

    const auto& children = ws->GetChildren();
    for (size_t i = 0; i < children.size(); ++i) {
      print_node(children[i], depth + 1, i == children.size() - 1);
    }
  };

  print_node(root, 0, true);
}

std::shared_ptr<Workspace> ForkMergeService::FindWorkspace(const std::string& id_or_branch) const {
  auto ws = root_workspace_;
  if (!ws) return nullptr;
  std::shared_ptr<Workspace> found = nullptr;
  std::vector<std::shared_ptr<Workspace>> queue = {ws};
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
  if (!root_workspace_) return 0;

  size_t total_removed = 0;
  std::function<void(std::shared_ptr<Workspace>&)> prune_recursive;
  prune_recursive = [&](std::shared_ptr<Workspace>& ws) {
    if (!ws) return;
    total_removed += ws->RemoveMergedChildren();
    auto children = ws->GetChildren();
    for (auto& child : children) {
      auto mutable_child = std::const_pointer_cast<Workspace>(child);
      prune_recursive(mutable_child);
    }
  };

  prune_recursive(root_workspace_);
  return total_removed;
}

std::vector<Artifact> ForkMergeService::ExtractArtifacts(const std::shared_ptr<Workspace>& ws,
                                                      const std::string& goal) {
  ArtifactExtractor extractor;
  return extractor.Extract(ws, goal);
}

HandoffReceipt ForkMergeService::GenerateSummary(const std::shared_ptr<Workspace>& child_workspace,
                                                  const Assignment& delegation,
                                                  LLMProvider* provider) {
  SummaryGenerator summary_gen(manager_);
  return summary_gen.Generate(child_workspace, delegation, provider);
}

}  // namespace pu