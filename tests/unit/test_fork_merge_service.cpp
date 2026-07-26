// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/fork_merge_service.hpp"
#include "pu/core/context.hpp"
#include "pu/core/delegation_stack.hpp"
#include "pu/agent_core.hpp"

#include <sstream>

using namespace pu::core;

// A minimal mock AgentManager for testing purposes
namespace {

class MockAgentManager : public pu::agent::AgentManager {
 public:
  MockAgentManager() : pu::agent::AgentManager() {}
  // Minimal mock - just enough to construct ForkMergeService
};

}  // namespace

TEST_CASE("ForkMergeService::Fork creates a child context", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  auto stack = std::make_shared<DelegationStack>(root);
  MockAgentManager manager;

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
  auto stack = std::make_shared<DelegationStack>(root);
  MockAgentManager manager;

  ForkMergeService service(manager, stack, root);

  auto result = service.Fork("test-agent", "Exploration", "");

  REQUIRE(result.child_context != nullptr);
  REQUIRE(result.child_context->GetBranchName().find("fork_") == 0);
}

TEST_CASE("ForkMergeService::PrintTree outputs tree structure", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  auto stack = std::make_shared<DelegationStack>(root);
  MockAgentManager manager;

  ForkMergeService service(manager, stack, root);

  // Fork a child
  auto fork_result = service.Fork("agent1", "Goal 1", "branch1");
  REQUIRE(fork_result.child_context != nullptr);

  std::ostringstream oss;
  service.PrintTree(oss);
  std::string output = oss.str();

  // Should contain the default branch name "main" and the child branch
  REQUIRE(output.find("main") != std::string::npos);
  REQUIRE(output.find("branch1") != std::string::npos);
  REQUIRE(output.find("=== Fork Tree ===") != std::string::npos);
}

TEST_CASE("ForkMergeService::FindContext finds by id or branch name", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  auto stack = std::make_shared<DelegationStack>(root);
  MockAgentManager manager;

  ForkMergeService service(manager, stack, root);

  auto fork_result = service.Fork("agent1", "Goal", "my-branch");
  REQUIRE(fork_result.child_context != nullptr);

  // Find by branch name
  auto found = service.FindContext("my-branch");
  REQUIRE(found != nullptr);
  REQUIRE(found->GetBranchName() == "my-branch");

  // Find by id
  auto id = fork_result.child_context->GetId();
  auto found_by_id = service.FindContext(id);
  REQUIRE(found_by_id != nullptr);
  REQUIRE(found_by_id->GetId() == id);
}

TEST_CASE("ForkMergeService::FindContext returns null for unknown", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  auto stack = std::make_shared<DelegationStack>(root);
  MockAgentManager manager;

  ForkMergeService service(manager, stack, root);

  auto found = service.FindContext("nonexistent-branch");
  REQUIRE(found == nullptr);
}

TEST_CASE("ForkMergeService::PruneMerged removes merged children", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  auto stack = std::make_shared<DelegationStack>(root);
  MockAgentManager manager;

  ForkMergeService service(manager, stack, root);

  // Fork two children
  auto fork1 = service.Fork("agent1", "Task 1", "branch1");
  auto fork2 = service.Fork("agent2", "Task 2", "branch2");

  // Merge the first one via manual context operations
  auto parent = fork1.child_context->GetParent();
  REQUIRE(parent != nullptr);
  parent->Merge(fork1.child_context, "Merged branch1");

  // Now prune merged branches
  size_t removed = service.PruneMerged();
  // Should remove 1 merged branch
  REQUIRE(removed > 0);

  // Only branch2 should remain
  auto remaining = service.FindContext("branch2");
  REQUIRE(remaining != nullptr);
}

TEST_CASE("ForkMergeService::ExtractFacts extracts file paths and errors", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  auto stack = std::make_shared<DelegationStack>(root);
  MockAgentManager manager;

  ForkMergeService service(manager, stack, root);

  // Add some messages with file paths and errors
  root->Append("user", "Check /home/user/file.cpp for issues");
  root->Append("assistant", "I found an error in the configuration");
  root->Append("user", "The build failed with an error");

  auto facts = service.ExtractFacts(root, "testing");

  // Should find file paths and error messages
  bool found_file = false;
  bool found_error = false;
  for (const auto& f : facts) {
    if (f.type == Fact::Type::kFilePath) found_file = true;
    if (f.type == Fact::Type::kErrorMsg) found_error = true;
  }

  REQUIRE(found_file);
  REQUIRE(found_error);
}

TEST_CASE("ForkMergeService::GetRootContext returns root", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  auto stack = std::make_shared<DelegationStack>(root);
  MockAgentManager manager;

  ForkMergeService service(manager, stack, root);

  auto retrieved = service.GetRootContext();
  REQUIRE(retrieved != nullptr);
  REQUIRE(retrieved->GetId() == root->GetId());
}

TEST_CASE("ForkMergeService::GetDelegationStack returns stack", "[fork_merge_service]") {
  auto root = std::make_shared<Context>("root");
  auto stack = std::make_shared<DelegationStack>(root);
  MockAgentManager manager;

  ForkMergeService service(manager, stack, root);

  auto retrieved = service.GetDelegationStack();
  REQUIRE(retrieved != nullptr);
  REQUIRE(retrieved->IsEmpty());
}
