// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/session/workspace.hpp"
#include "pu/session/artifact.hpp"
#include <nlohmann/json.hpp>

using namespace pu;

TEST_CASE("Workspace::Fork creates child with inherited data", "[fork_merge]") {
  auto parent = std::make_shared<Workspace>("parent");
  parent->Append("user", "Hello");
  parent->Append("assistant", "Hi there!");
  parent->SetVar("theme", nlohmann::json("dark"));
  Artifact a;
  a.type = Artifact::Type::kFilePath;
  a.content = "/tmp/file.cpp";
  a.source = "user";
  parent->AddArtifact(a);

  auto child = parent->Fork("my-fork");

  REQUIRE(child != nullptr);
  REQUIRE(child->GetParent() == parent);
  REQUIRE(child->GetBranchName() == "my-fork");
  REQUIRE(child->GetState() == Workspace::State::kActive);

  // Child should inherit vars
  auto theme = child->GetVar("theme");
  REQUIRE(theme.has_value());
  REQUIRE(theme->get<std::string>() == "dark");

  // Child should inherit artifacts
  REQUIRE(child->GetArtifacts().size() == 1);
  REQUIRE(child->GetArtifacts()[0].content == "/tmp/file.cpp");

  // Child should have its own history (fork message only)
  REQUIRE(child->HistorySize() >= 1);

  // Parent should have registered the child
  REQUIRE(parent->GetChildren().size() == 1);
  REQUIRE(parent->GetChildren()[0] == child);
}

TEST_CASE("Workspace::Fork with empty branch name generates one", "[fork_merge]") {
  auto parent = std::make_shared<Workspace>("parent");
  auto child = parent->Fork("");

  REQUIRE(child != nullptr);
  REQUIRE(child->GetBranchName().find("fork_") == 0);
}

TEST_CASE("Workspace::Fork preserves data isolation", "[fork_merge]") {
  auto parent = std::make_shared<Workspace>("parent");
  parent->Append("user", "Parent message");

  auto child = parent->Fork("isolated");

  // Changes in child should not affect parent
  child->Append("assistant", "Child message");
  REQUIRE(child->HistorySize() == 2);
  REQUIRE(parent->HistorySize() == 1);

  // Changes in parent should not affect child
  parent->SetVar("only_parent", nlohmann::json("yes"));
  REQUIRE(child->HasVar("only_parent") == false);
}

TEST_CASE("Workspace::Merge creates merge workspace", "[fork_merge]") {
  auto parent = std::make_shared<Workspace>("parent");
  parent->Append("user", "Initial question");
  parent->SetVar("config", nlohmann::json("original"));

  auto child = parent->Fork("feature");
  child->Append("assistant", "Research findings");
  Artifact a;
  a.type = Artifact::Type::kFilePath;
  a.content = "/tmp/result.txt";
  a.source = "tool";
  child->AddArtifact(a);
  child->SetVar("result", nlohmann::json("success"));

  auto merge_ws = parent->Merge(child, "Task completed successfully");

  REQUIRE(merge_ws != nullptr);
  REQUIRE(merge_ws->IsMergeCommit() == true);
  REQUIRE(merge_ws->GetBranchName() == "main");

  // Merge should contain parent history
  REQUIRE(merge_ws->HistorySize() >= 2);
  auto history = merge_ws->GetHistory();
  bool found_parent_msg = false;
  bool found_merge_msg = false;
  for (const auto& msg : history) {
    if (msg.content.find("Initial question") != std::string::npos)
      found_parent_msg = true;
    if (msg.content.find("Task completed") != std::string::npos)
      found_merge_msg = true;
  }
  REQUIRE(found_parent_msg);
  REQUIRE(found_merge_msg);

  // Merge should contain child's artifacts
  REQUIRE(merge_ws->GetArtifacts().size() > 0);

  // Child should be marked as merged
  REQUIRE(child->GetState() == Workspace::State::kMerged);

  // Merge parents should be recorded
  auto parents = merge_ws->GetMergeParents();
  REQUIRE(parents.size() == 2);
}

TEST_CASE("Workspace::Merge fails on non-child workspace", "[fork_merge]") {
  auto parent = std::make_shared<Workspace>("parent");
  auto other = std::make_shared<Workspace>("other");
  auto child = parent->Fork("valid-child");

  REQUIRE_THROWS_AS(parent->Merge(other, "invalid merge"), std::runtime_error);
  REQUIRE_NOTHROW(parent->Merge(child, "valid merge"));
}

TEST_CASE("Workspace::Merge fails on already merged child", "[fork_merge]") {
  auto parent = std::make_shared<Workspace>("parent");
  auto child = parent->Fork("child");
  parent->Merge(child, "first merge");

  REQUIRE_THROWS_AS(parent->Merge(child, "second merge"), std::runtime_error);
}

TEST_CASE("Workspace::GetTokenCount returns estimate", "[fork_merge]") {
  auto ctx = std::make_shared<Workspace>("test");
  ctx->Append("user", "Hello world");
  ctx->Append("assistant", "Hi there, how can I help you today?");

  size_t tokens = ctx->GetTokenCount();
  REQUIRE(tokens > 0);
  REQUIRE(tokens < 20);
}

TEST_CASE("Fork-Merge tree structure", "[fork_merge]") {
  auto root = std::make_shared<Workspace>("root");
  root->Append("system", "Starting analysis");

  // Fork two children
  auto child1 = root->Fork("analyze-code");
  child1->Append("assistant", "Code analysis results");

  auto child2 = root->Fork("check-tests");
  child2->Append("assistant", "Test results");

  // Merge first child
  auto merge1 = root->Merge(child1, "Code analysis complete");

  // Fork another from merge
  auto child3 = merge1->Fork("fix-issues");
  child3->Append("assistant", "Fixing issues");

  // Verify tree structure
  REQUIRE(root->GetChildren().size() == 2);
  REQUIRE(merge1->GetChildren().size() == 1);
  REQUIRE(merge1->GetChildren()[0] == child3);

  // Verify state
  REQUIRE(child1->GetState() == Workspace::State::kMerged);
  REQUIRE(child2->GetState() == Workspace::State::kActive);
  REQUIRE(child3->GetState() == Workspace::State::kActive);
  REQUIRE(merge1->IsMergeCommit());
}

TEST_CASE("Workspace::RemoveMergedChildren removes merged children", "[fork_merge]") {
  auto parent = std::make_shared<Workspace>("parent");
  auto active = parent->Fork("active");
  auto merged = parent->Fork("merged");
  parent->Merge(merged, "merge it");

  REQUIRE(parent->GetChildren().size() == 2);

  size_t removed = parent->RemoveMergedChildren();
  REQUIRE(removed == 1);
  REQUIRE(parent->GetChildren().size() == 1);
  REQUIRE(parent->GetChildren()[0] == active);
}
