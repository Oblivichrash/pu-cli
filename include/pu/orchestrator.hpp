// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <ostream>
#include <string>

#include "pu/agent_core.hpp"
#include "pu/core/delegation_stack.hpp"
#include "pu/core/context.hpp"

namespace pu {

class Orchestrator {
 public:
  Orchestrator(agent::AgentManager& manager);

  void SetDelegationStack(std::shared_ptr<core::DelegationStack> stack);

  bool HandleCommand(const std::string& input, std::string& output);
  std::string Process(const std::string& input);

  bool PushDelegation(const std::string& agent_name, const std::string& goal);
  core::SummaryReport PopDelegation();

  void SetMaxDepth(int depth) { max_depth_ = depth; }

  // Fork-Merge Methods

  /// Fork a new context for sub-task execution.
  /// Returns the child context.
  std::shared_ptr<core::Context> ForkContext(
      const std::string& agent_name,
      const std::string& goal,
      const std::string& branch_name = "");

  /// Merge a child context back to parent.
  /// Returns the merge summary report.
  core::SummaryReport MergeContext(
      const std::string& message,
      const std::string& strategy = "merge");

  /// Print ASCII fork tree to output stream
  void PrintForkTree(std::ostream& os);

  /// Remove all merged forks from the tree
  size_t PruneMergedForks();

 private:
  core::FactList ExtractFacts(const std::shared_ptr<core::Context>& ctx,
                              const std::string& goal);

  core::SummaryReport GenerateSummary(const std::shared_ptr<core::Context>& child_ctx,
                                      const core::Delegation& delegation);

  void InjectSummaryIntoParent(const core::SummaryReport& report);

  std::shared_ptr<core::DelegationStack> delegation_stack_;
  std::shared_ptr<core::Context> root_context_;
  agent::AgentManager& manager_;

  int max_depth_ = 5;
};

}  // namespace pu
