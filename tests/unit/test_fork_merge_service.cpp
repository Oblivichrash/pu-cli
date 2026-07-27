// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/fork_merge_service.hpp"
#include "pu/core/context.hpp"
#include "pu/core/delegation_stack.hpp"
#include "pu/agent_core.hpp"

#include <sstream>

using namespace pu::core;

namespace {

class MockAgentManager : public pu::agent::AgentManager {
 public:
  MockAgentManager() : pu::agent::AgentManager() {}
};

}  // namespace

TEST_CASE("ForkMergeService::Fork creates a child context", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  MockAgentManager manager;
  auto stack = DelegationStack::Create(root, manager);

  ForkMergeService service(manager, stack, root);

  auto result = service.Fork("test-agent", "Test exploration", "test-branch");

  REQUIRE(result.child_context != nullptr);
  REQUIRE(result.child_context->GetBranchName() == "test-branch");
  REQUIRE(result.child_context->GetParent() == root);
  REQUIRE(result.message.find("Forked to branch") != std::string::npos);
  REQUIRE(result.message.find("test-branch") != std::string::npos);
}

TEST_CASE("ForkMergeService::Fork without branch name generates one", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  MockAgentManager manager;
  auto stack = DelegationStack::Create(root, manager);

  ForkMergeService service(manager, stack, root);

  auto result = service.Fork("test-agent", "Exploration", "");

  REQUIRE(result.child_context != nullptr);
  REQUIRE(result.child_context->GetBranchName().find("fork_") == 0);
}

TEST_CASE("ForkMergeService::PrintTree outputs tree structure", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  MockAgentManager manager;
  auto stack = DelegationStack::Create(root, manager);

  ForkMergeService service(manager, stack, root);

  auto fork_result = service.Fork("agent1", "Goal 1", "branch1");

  std::ostringstream os;
  service.PrintTree(os);
  std::string output = os.str();

  REQUIRE(output.find("main") != std::string::npos);
  REQUIRE(output.find("branch1") != std::string::npos);
}

TEST_CASE("ForkMergeService::Merge with squash strategy", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  MockAgentManager manager;
  auto stack = DelegationStack::Create(root, manager);

  ForkMergeService service(manager, stack, root);

  auto fork_result = service.Fork("agent1", "Goal 1", "branch1");
  auto child = fork_result.child_context;

  Delegation deleg("Goal 1", "agent1", {}, 0);
  deleg.id = Delegation::GenerateId();
  stack->Push(deleg, child);

  child->Append("assistant", "Working on task");
  child->Append("assistant", "Done");

  auto merge_result = service.Merge("Task completed", "squash");

  REQUIRE(merge_result.report.status == SummaryReport::Status::kCompleted);
  REQUIRE(!merge_result.report.summary.empty());
  REQUIRE(merge_result.merge_context != nullptr);
}

TEST_CASE("ForkMergeService::Merge with merge strategy", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  MockAgentManager manager;
  auto stack = DelegationStack::Create(root, manager);

  ForkMergeService service(manager, stack, root);

  auto fork_result = service.Fork("agent1", "Goal 1", "branch1");
  auto child = fork_result.child_context;

  Delegation deleg("Goal 1", "agent1", {}, 0);
  deleg.id = Delegation::GenerateId();
  stack->Push(deleg, child);

  child->Append("assistant", "Working on task");
  child->Append("assistant", "Done");

  auto merge_result = service.Merge("Task completed", "merge");

  REQUIRE(merge_result.report.status == SummaryReport::Status::kCompleted);
  REQUIRE(!merge_result.report.summary.empty());
  REQUIRE(merge_result.merge_context != nullptr);
}

TEST_CASE("ForkMergeService::ExtractFacts extracts facts from context", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  MockAgentManager manager;
  auto stack = DelegationStack::Create(root, manager);

  ForkMergeService service(manager, stack, root);

  root->Append("user", "Check /tmp/test.cpp for issues");
  root->Append("system", "error: something failed");

  auto facts = service.ExtractFacts(root, "review code");

  REQUIRE(facts.size() >= 1);
  bool found_file = false;
  for (const auto& f : facts) {
    if (f.type == Fact::Type::kFilePath && f.content == "/tmp/test.cpp") {
      found_file = true;
      break;
    }
  }
  REQUIRE(found_file);
}

TEST_CASE("ForkMergeService::GenerateSummary generates summary", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  MockAgentManager manager;
  auto stack = DelegationStack::Create(root, manager);

  ForkMergeService service(manager, stack, root);

  auto child = std::make_shared<Context>("child");
  child->Append("user", "Please review this code");
  child->Append("assistant", "I'll review it");
  child->Append("assistant", "Found an issue on line 42");

  Delegation deleg("review code", "code-agent", {}, 0);
  deleg.id = Delegation::GenerateId();

  auto report = service.GenerateSummary(child, deleg);

  REQUIRE(report.status == SummaryReport::Status::kCompleted);
  REQUIRE(!report.summary.empty());
}

TEST_CASE("ForkMergeService::InjectSummaryIntoParent injects summary", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  MockAgentManager manager;
  auto stack = DelegationStack::Create(root, manager);

  ForkMergeService service(manager, stack, root);

  // Ensure stack is empty (initial state)
  REQUIRE(stack->IsEmpty());

  // Construct a summary report manually
  SummaryReport report;
  report.status = SummaryReport::Status::kCompleted;
  report.summary = "Test summary from unit test";

  // Inject summary (stack empty => inject to root)
  service.InjectSummaryIntoParent(report);

  // Verify root history contains the summary
  bool found = false;
  for (const auto& msg : root->GetHistory()) {
    if (msg.role == "system" && msg.content.find("Test summary") != std::string::npos) {
      found = true;
      break;
    }
  }
  REQUIRE(found);
}

TEST_CASE("ForkMergeService::PruneMerged removes merged forks", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  MockAgentManager manager;
  auto stack = DelegationStack::Create(root, manager);

  ForkMergeService service(manager, stack, root);

  // Manually fork and merge using Context API (not via ForkMergeService)
  auto child = root->Fork("branch1");
  root->Merge(child, "Merged branch1");

  // Now child is marked as merged and is still in root's children list
  // PruneMerged should remove it
  size_t pruned = service.PruneMerged();
  REQUIRE(pruned == 1);
  REQUIRE(root->GetChildren().size() == 0);
}

TEST_CASE("ForkMergeService::PopDelegation", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  MockAgentManager manager;
  auto stack = DelegationStack::Create(root, manager);

  ForkMergeService service(manager, stack, root);

  Delegation d("goal", "agent", {}, 0);
  d.id = Delegation::GenerateId();
  auto ctx = std::make_shared<Context>("ctx");
  stack->Push(d, ctx);

  auto report = service.PopDelegation();

  REQUIRE(report.status == SummaryReport::Status::kCompleted);
  REQUIRE(stack->IsEmpty());
}
