// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/fork_merge_service.hpp"
#include "pu/session/workspace.hpp"
#include "pu/agent/agent_manager.hpp"

#include <sstream>

using namespace pu;

namespace {

class MockAgentManager : public pu::AgentManager {
 public:
  MockAgentManager() : pu::AgentManager() {}
};

}  // namespace

TEST_CASE("ForkMergeService::Fork creates a child workspace", "[fork_merge_service]") {
  auto root = std::make_shared<Workspace>("root");
  MockAgentManager manager;

  ForkMergeService service(manager, root);

  auto result = service.Fork(root, "test-agent", "Test exploration", "test-branch");

  REQUIRE(result.child_workspace != nullptr);
  REQUIRE(result.child_workspace->GetBranchName() == "test-branch");
  REQUIRE(result.child_workspace->GetParent() == root);
}

TEST_CASE("ForkMergeService::Fork creates a child with default branch name", "[fork_merge_service]") {
  auto root = std::make_shared<Workspace>("root");
  MockAgentManager manager;

  ForkMergeService service(manager, root);

  auto result = service.Fork(root, "test-agent", "Test exploration", "");

  REQUIRE(result.child_workspace != nullptr);
  REQUIRE_FALSE(result.child_workspace->GetBranchName().empty());
  REQUIRE(result.child_workspace->GetParent() == root);
}

TEST_CASE("ForkMergeService Fork/Prune", "[fork_merge_service]") {
  auto root = std::make_shared<Workspace>("root");
  MockAgentManager manager;

  ForkMergeService service(manager, root);

  auto fork1 = service.Fork(root, "agent1", "Explore A", "branch-a");
  REQUIRE(fork1.child_workspace != nullptr);

  auto fork2 = service.Fork(root, "agent2", "Explore B", "branch-b");
  REQUIRE(fork2.child_workspace != nullptr);

  REQUIRE(root->GetChildren().size() == 2);

  // Prune should not remove active forks
  REQUIRE(service.PruneMerged() == 0);
  REQUIRE(root->GetChildren().size() == 2);
}

TEST_CASE("ForkMergeService::Merge basic merge", "[fork_merge_service]") {
  auto root = std::make_shared<Workspace>("root");
  MockAgentManager manager;

  ForkMergeService service(manager, root);

  auto fork = service.Fork(root, "agent1", "Test merge", "merge-test");
  REQUIRE(fork.child_workspace != nullptr);

  auto merge = service.Merge(fork.child_workspace, "Merging test branch", "merge");
  REQUIRE(merge.merge_workspace != nullptr);
  REQUIRE(merge.report.summary.find("Merging") != std::string::npos);

  // After merge, the child is marked merged and remains a child of root;
  // the merge context is a separate workspace (not grafted into the tree).
  // PruneMerged() therefore removes exactly the one merged branch.
  REQUIRE(fork.child_workspace->GetState() == Workspace::State::kMerged);
  REQUIRE(service.PruneMerged() == 1);
  REQUIRE(root->GetChildren().empty());
}

TEST_CASE("ForkMergeService::PrintTree produces output", "[fork_merge_service]") {
  auto root = std::make_shared<Workspace>("root");
  MockAgentManager manager;

  ForkMergeService service(manager, root);

  auto fork = service.Fork(root, "agent1", "Print test", "print-branch");
  REQUIRE(fork.child_workspace != nullptr);

  std::ostringstream oss;
  service.PrintTree(oss);
  REQUIRE(oss.str().find("print-branch") != std::string::npos);
  REQUIRE(oss.str().find("main") != std::string::npos);
}

TEST_CASE("ForkMergeService::FindWorkspace finds by id", "[fork_merge_service]") {
  auto root = std::make_shared<Workspace>("root");
  MockAgentManager manager;

  ForkMergeService service(manager, root);

  auto fork = service.Fork(root, "agent1", "Find test", "find-branch");
  REQUIRE(fork.child_workspace != nullptr);

  auto found = service.FindWorkspace(fork.child_workspace->GetId());
  REQUIRE(found != nullptr);
  REQUIRE(found->GetBranchName() == "find-branch");
}

TEST_CASE("ForkMergeService::FindWorkspace returns null for non-existent", "[fork_merge_service]") {
  auto root = std::make_shared<Workspace>("root");
  MockAgentManager manager;

  ForkMergeService service(manager, root);

  auto found = service.FindWorkspace("nonexistent-id");
  REQUIRE(found == nullptr);
}

TEST_CASE("ForkMergeService multiple forks and merges (sequential)", "[fork_merge_service]") {
  auto root = std::make_shared<Workspace>("root");
  MockAgentManager manager;

  ForkMergeService service(manager, root);

  // Fork and merge branch A
  auto fork1 = service.Fork(root, "agent1", "Task A", "branch-a");
  REQUIRE(fork1.child_workspace != nullptr);

  auto merge1 = service.Merge(fork1.child_workspace, "Completed A", "merge");
  REQUIRE(merge1.merge_workspace != nullptr);

  // Fork and merge branch B (from the merged root)
  auto fork2 = service.Fork(merge1.merge_workspace, "agent2", "Task B", "branch-b");
  REQUIRE(fork2.child_workspace != nullptr);

  auto merge2 = service.Merge(fork2.child_workspace, "Completed B", "merge");
  REQUIRE(merge2.merge_workspace != nullptr);
}

TEST_CASE("ForkMergeService GetRootWorkspace", "[fork_merge_service]") {
  auto root = std::make_shared<Workspace>("root");
  MockAgentManager manager;

  ForkMergeService service(manager, root);

  REQUIRE(service.GetRootWorkspace() == root);
}
