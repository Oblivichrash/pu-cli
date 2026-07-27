// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/session/workspace.hpp"
#include "pu/session/call_stack.hpp"
#include "pu/agent_core.hpp"

#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace pu {

class ForkMergeService {
 public:
  struct ForkResult {
    std::shared_ptr<Workspace> child_context;
    std::string message;
  };

  struct MergeResult {
    std::shared_ptr<Workspace> merge_context;
    HandoffReceipt report;
    std::string message;
  };

  struct ForkContextResult {
    std::shared_ptr<Workspace> child_context;
    std::string message;
  };

  ForkMergeService(agent::AgentManager& manager,
                   std::shared_ptr<CallStack> delegation_stack,
                   std::shared_ptr<Workspace> root_context);

  // Fork operations
  ForkResult Fork(const std::string& agent_name,
                  const std::string& goal,
                  const std::string& branch_name = "");

  // Merge operations
  MergeResult Merge(const std::string& message,
                    const std::string& strategy = "merge");

  // Tree operations
  void PrintTree(std::ostream& os) const;

  // Workspace lookup
  std::shared_ptr<Workspace> FindContext(const std::string& id_or_branch) const;

  // Pruning
  size_t PruneMerged();

  // Artifact extraction
  std::vector<Artifact> ExtractFacts(const std::shared_ptr<Workspace>& ctx,
                         const std::string& goal);

  // Summary generation
  HandoffReceipt GenerateSummary(const std::shared_ptr<Workspace>& child_ctx,
                                 const Assignment& delegation);

  // Summary injection
  void InjectSummaryIntoParent(const HandoffReceipt& report);

  // Pop delegation
  HandoffReceipt PopDelegation();

  // Accessors
  std::shared_ptr<Workspace> GetRootContext() const { return root_context_; }
  std::shared_ptr<CallStack> GetDelegationStack() const { return delegation_stack_; }

 private:
  void PrintTree(std::ostream& os, const std::shared_ptr<Workspace>& root) const;

  agent::AgentManager& manager_;
  std::shared_ptr<CallStack> delegation_stack_;
  std::shared_ptr<Workspace> root_context_;
};

}  // namespace pu
