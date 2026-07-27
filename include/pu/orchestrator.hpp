// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <ostream>
#include <string>

#include "pu/agent_core.hpp"
#include "pu/session/call_stack.hpp"
#include "pu/session/workspace.hpp"

namespace pu {
class CommandHandler;
class ForkMergeService;
}

namespace pu {

class Orchestrator {
 public:
  Orchestrator(agent::AgentManager& manager);

  void SetDelegationStack(std::shared_ptr<CallStack> stack);

  bool HandleCommand(const std::string& input, std::string& output);
  std::string Process(const std::string& input);

  bool PushDelegation(const std::string& agent_name, const std::string& goal);
  HandoffReceipt PopDelegation();

  void SetMaxDepth(int depth) { max_depth_ = depth; }

  // Delegated to ForkMergeService (via CallStack)
  std::shared_ptr<Workspace> ForkContext(
      const std::string& agent_name,
      const std::string& goal,
      const std::string& branch_name = "");

  HandoffReceipt MergeContext(
      const std::string& message,
      const std::string& strategy = "merge");

  void PrintForkTree(std::ostream& os);

  size_t PruneMergedForks();

 private:
  std::vector<Artifact> ExtractFacts(const std::shared_ptr<Workspace>& ctx,
                              const std::string& goal);

  HandoffReceipt GenerateSummary(const std::shared_ptr<Workspace>& child_ctx,
                                      const Assignment& delegation);

  void InjectSummaryIntoParent(const HandoffReceipt& report);

  std::shared_ptr<CallStack> delegation_stack_;
  std::shared_ptr<Workspace> root_context_;
  agent::AgentManager& manager_;
  std::shared_ptr<CommandHandler> command_handler_;

  int max_depth_ = 5;
};

}  // namespace pu
