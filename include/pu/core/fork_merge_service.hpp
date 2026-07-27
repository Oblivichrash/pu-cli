// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/core/context.hpp"
#include "pu/core/delegation_stack.hpp"
#include "pu/agent_core.hpp"

#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace pu::core {

class ForkMergeService {
 public:
  struct ForkResult {
    std::shared_ptr<Context> child_context;
    std::string message;
  };

  struct MergeResult {
    std::shared_ptr<Context> merge_context;
    SummaryReport report;
    std::string message;
  };

  struct ForkContextResult {
    std::shared_ptr<Context> child_context;
    std::string message;
  };

  ForkMergeService(agent::AgentManager& manager,
                   std::shared_ptr<DelegationStack> delegation_stack,
                   std::shared_ptr<Context> root_context);

  // Fork operations
  ForkResult Fork(const std::string& agent_name,
                  const std::string& goal,
                  const std::string& branch_name = "");

  // Merge operations
  MergeResult Merge(const std::string& message,
                    const std::string& strategy = "merge");

  // Tree operations
  void PrintTree(std::ostream& os) const;

  // Context lookup
  std::shared_ptr<Context> FindContext(const std::string& id_or_branch) const;

  // Pruning
  size_t PruneMerged();

  // Fact extraction
  FactList ExtractFacts(const std::shared_ptr<Context>& ctx,
                         const std::string& goal);

  // Summary generation
  SummaryReport GenerateSummary(const std::shared_ptr<Context>& child_ctx,
                                 const Delegation& delegation);

  // Summary injection
  void InjectSummaryIntoParent(const SummaryReport& report);

  // Pop delegation
  SummaryReport PopDelegation();

  // Accessors
  std::shared_ptr<Context> GetRootContext() const { return root_context_; }
  std::shared_ptr<DelegationStack> GetDelegationStack() const { return delegation_stack_; }

 private:
  void PrintTree(std::ostream& os, const std::shared_ptr<Context>& root) const;

  agent::AgentManager& manager_;
  std::shared_ptr<DelegationStack> delegation_stack_;
  std::shared_ptr<Context> root_context_;
};

}  // namespace pu::core