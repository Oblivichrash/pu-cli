// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/session/workspace.hpp"
#include "pu/session/assignment.hpp"
#include "pu/session/handoff_receipt.hpp"
#include "pu/agent/agent_manager.hpp"
#include "pu/llm/llm_provider.hpp"

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

  ForkMergeService(AgentManager& manager,
                   std::shared_ptr<Workspace> root_context);

  // Fork operations — caller provides the parent workspace directly.
  ForkResult Fork(const std::shared_ptr<Workspace>& parent,
                  const std::string& agent_name,
                  const std::string& goal,
                  const std::string& branch_name = "");

  // Merge operations — caller provides the child workspace directly.
  MergeResult Merge(const std::shared_ptr<Workspace>& child,
                    const std::string& message,
                    const std::string& strategy = "merge",
                    LLMProvider* provider = nullptr);

  // Tree operations
  void PrintTree(std::ostream& os) const;

  // Workspace lookup
  std::shared_ptr<Workspace> FindWorkspace(const std::string& id_or_branch) const;

  // Pruning
  size_t PruneMerged();

  // Artifact extraction
  std::vector<Artifact> ExtractArtifacts(const std::shared_ptr<Workspace>& ctx,
                         const std::string& goal);

  // Summary generation
  HandoffReceipt GenerateSummary(const std::shared_ptr<Workspace>& child_ctx,
                                 const Assignment& delegation,
                                 LLMProvider* provider = nullptr);

  // Accessors
  std::shared_ptr<Workspace> GetRootWorkspace() const { return root_context_; }

 private:
  void PrintTree(std::ostream& os, const std::shared_ptr<Workspace>& root) const;

  AgentManager& manager_;
  std::shared_ptr<Workspace> root_context_;
};

}  // namespace pu