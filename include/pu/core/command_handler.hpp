// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>

#include "pu/agent_core.hpp"
#include "pu/session/call_stack.hpp"
#include "pu/core/fork_merge_service.hpp"

namespace pu {

class CommandHandler {
 public:
  CommandHandler(agent::AgentManager& manager,
                 std::shared_ptr<ForkMergeService> fork_service,
                 std::shared_ptr<CallStack> delegation_stack,
                 std::shared_ptr<Workspace> root_context);

  bool Handle(const std::string& input, std::string& output);

  void SetMaxDepth(int depth) { max_depth_ = depth; }

 private:
  // Fork commands
  bool HandleFork(const std::string& args, std::string& output);
  bool HandleForkShow(const std::string& fork_id, std::string& output);
  bool HandleForkPrune(const std::string& args, std::string& output);

  // Push/pop commands
  bool HandlePush(const std::string& args, std::string& output);
  bool HandlePop(std::string& output);

  // Stack command
  bool HandleStack(std::string& output);

  // Assignment helpers
  std::vector<Artifact> ExtractFacts(const std::shared_ptr<Workspace>& ctx,
                              const std::string& goal);
  HandoffReceipt GenerateSummary(const std::shared_ptr<Workspace>& child_ctx,
                                      const Assignment& delegation);
  void InjectSummaryIntoParent(const HandoffReceipt& report);

  agent::AgentManager& manager_;
  std::shared_ptr<ForkMergeService> fork_service_;
  std::shared_ptr<CallStack> delegation_stack_;
  std::shared_ptr<Workspace> root_context_;
  int max_depth_ = 5;
};

}  // namespace pu
