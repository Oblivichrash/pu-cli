// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/command_handler.hpp"
#include "pu/executor.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <sstream>

namespace pu::core {

CommandHandler::CommandHandler(agent::AgentManager& manager,
                               std::shared_ptr<core::ForkMergeService> fork_service,
                               std::shared_ptr<core::DelegationStack> delegation_stack,
                               std::shared_ptr<core::Context> root_context)
    : manager_(manager),
      fork_service_(std::move(fork_service)),
      delegation_stack_(std::move(delegation_stack)),
      root_context_(std::move(root_context)) {}

bool CommandHandler::Handle(const std::string& input, std::string& output) {
  if (input.rfind("/fork", 0) == 0) {
    std::string args = input.substr(5);
    size_t start = args.find_first_not_of(" \t");
    if (start != std::string::npos) args = args.substr(start);
    else args.clear();
    return HandleFork(args, output);
  }

  if (input.rfind("/push ", 0) == 0) {
    return HandlePush(input.substr(6), output);
  }

  if (input == "/pop") {
    return HandlePop(output);
  }

  if (input == "/stack") {
    return HandleStack(output);
  }

  return false;
}

bool CommandHandler::HandleFork(const std::string& args, std::string& output) {
  if (args.empty() || args == "list") {
    std::ostringstream oss;
    if (fork_service_) {
      fork_service_->PrintTree(oss);
    } else {
      oss << "No fork service available.\n";
    }
    output = oss.str();
    return true;
  }

  if (args.rfind("show ", 0) == 0) {
    return HandleForkShow(args.substr(5), output);
  }

  if (args.rfind("prune", 0) == 0) {
    return HandleForkPrune(args, output);
  }

  output = "Usage: /fork [<agent>] | /fork list | /fork show <id> | /fork prune";
  return true;
}

bool CommandHandler::HandleForkShow(const std::string& fork_id, std::string& output) {
  if (fork_id.empty()) { output = "Usage: /fork show <id>"; return true; }
  if (!fork_service_) { output = "No fork service available."; return true; }
  auto found = fork_service_->FindContext(fork_id);
  if (!found) { output = "Context not found: " + fork_id; return true; }
  std::ostringstream oss;
  auto st = found->GetState();
  oss << "=== Context: " << found->GetId() << " ===\n";
  oss << "  Branch: " << found->GetBranchName() << "\n";
  oss << "  State: " << (st == core::Context::State::kActive ? "active" :
                         st == core::Context::State::kMerged ? "merged" : "abandoned") << "\n";
  oss << "  History: " << found->HistorySize() << " messages\n";
  oss << "  Tokens: ~" << found->GetTokenCount() << "\n";
  oss << "  Facts: " << found->GetFacts().size() << "\n";
  oss << "  Vars: " << found->GetAllVars().size() << "\n";
  if (found->IsMergeCommit()) {
    oss << "  Merge commit: yes\n";
    oss << "  Parents: " << found->GetMergeParents().size() << "\n";
  }
  auto parent = found->GetParent();
  if (parent) oss << "  Parent: " << parent->GetId() << "\n";
  oss << "  Children: " << found->GetChildren().size() << "\n";
  auto recent = found->Recent(10);
  if (!recent.empty()) {
    oss << "\n  Recent messages:\n";
    for (const auto& msg : recent) {
      std::string preview = msg.content.substr(0, 100);
      oss << "    [" << msg.role << "] " << preview;
      if (msg.content.size() > 100) oss << "...";
      oss << "\n";
    }
  }
  output = oss.str();
  return true;
}

bool CommandHandler::HandleForkPrune(const std::string& args, std::string& output) {
  bool confirmed = (args.find("--yes") != std::string::npos ||
                    args.find("-y") != std::string::npos);
  size_t count = fork_service_ ? fork_service_->PruneMerged() : 0;
  std::ostringstream oss;
  if (confirmed) {
    oss << "Pruned " << count << " merged branch(es).\n";
  } else {
    oss << "Found " << count << " merged branch(es). "
        << "Use /fork prune --yes to remove them.\n";
  }
  output = oss.str();
  return true;
}

bool CommandHandler::HandlePush(const std::string& args, std::string& output) {
  output = "\xe2\x9a\xa0\xef\xb8\x8f '/push' is deprecated. Please use '/fork <agent>' instead.\n";
  size_t space = args.find(' ');
  if (space == std::string::npos) {
    output = "Usage: /push <agent> \"<goal>\"";
    return true;
  }
  std::string agent_name = args.substr(0, space);
  std::string goal = args.substr(space + 1);
  if (goal.size() >= 2 && goal.front() == '"' && goal.back() == '"') {
    goal = goal.substr(1, goal.size() - 2);
  }
  if (goal.empty()) { output = "Error: goal cannot be empty"; return true; }
  int current_depth = delegation_stack_
      ? static_cast<int>(delegation_stack_->Depth()) : 0;
  if (current_depth >= max_depth_) {
    output = "Error: delegation depth limit reached ("
             + std::to_string(max_depth_) + ")";
    return true;
  }
  auto facts = ExtractFacts(root_context_, goal);
  core::Delegation deleg(goal, agent_name, facts, current_depth);
  deleg.id = core::Delegation::GenerateId();
  deleg.created_at = std::chrono::steady_clock::now();
  deleg.deadline = deleg.created_at + std::chrono::seconds(60);
  auto parent = delegation_stack_
      ? delegation_stack_->CurrentContext() : root_context_;
  std::shared_ptr<core::Context> child_ctx;
  if (parent) {
    child_ctx = parent->Fork("deleg-" + deleg.id);
    child_ctx->AddFacts(facts);
    child_ctx->Append("system", "Delegation started: " + goal);
  } else {
    child_ctx = std::make_shared<core::Context>("ctx-" + deleg.id);
    child_ctx->AddFacts(facts);
    child_ctx->Append("system", "Delegation started: " + goal);
  }
  delegation_stack_->Push(deleg, child_ctx);
  output = "Pushed agent: " + agent_name + " with goal: " + goal;
  return true;
}

bool CommandHandler::HandlePop(std::string& output) {
  output = "\xe2\x9a\xa0\xef\xb8\x8f '/pop' is deprecated. Please use '/merge' instead.\n";
  if (!delegation_stack_ || delegation_stack_->IsEmpty()) {
    output = "Error: no active delegation to pop";
    return true;
  }
  auto child = delegation_stack_->CurrentContext();
  auto parent = child ? child->GetParent() : nullptr;
  if (parent && fork_service_) {
    auto result = fork_service_->Merge("Task completed", "merge");
    InjectSummaryIntoParent(result.report);
    output = "Merged delegation: " + result.report.summary;
  } else if (fork_service_) {
    auto report = fork_service_->PopDelegation();
    InjectSummaryIntoParent(report);
    output = "Popped delegation: " + report.summary;
  } else {
    output = "Error: ForkMergeService not available";
  }
  return true;
}

bool CommandHandler::HandleStack(std::string& output) {
  if (delegation_stack_ && !delegation_stack_->IsEmpty()) {
    std::ostringstream oss;
    oss << "Delegation stack (depth " << delegation_stack_->Depth() << "):\n";
    for (const auto& frame : delegation_stack_->GetFrames()) {
      oss << "  " << frame.delegation.agent_name
          << " [" << frame.delegation.id << "]\n";
      if (frame.context) {
        oss << "    Context: " << frame.context->GetId()
            << " [branch: " << frame.context->GetBranchName() << "]\n";
      }
    }
    output = oss.str();
  } else if (delegation_stack_) {
    output = "Delegation stack is empty (depth 0)";
  } else {
    output = "No delegation stack active";
  }
  return true;
}

core::FactList CommandHandler::ExtractFacts(const std::shared_ptr<core::Context>& ctx,
                                            const std::string& goal) {
  if (fork_service_) {
    return fork_service_->ExtractFacts(ctx, goal);
  }
  return {};
}

core::SummaryReport CommandHandler::GenerateSummary(
    const std::shared_ptr<core::Context>& child_ctx,
    const core::Delegation& delegation) {
  if (fork_service_) {
    return fork_service_->GenerateSummary(child_ctx, delegation);
  }
  core::SummaryReport report;
  report.status = core::SummaryReport::Status::kFailed;
  report.summary = "ForkMergeService not initialized";
  return report;
}

void CommandHandler::InjectSummaryIntoParent(const core::SummaryReport& report) {
  if (fork_service_) {
    fork_service_->InjectSummaryIntoParent(report);
  }
}

}  // namespace pu::core
