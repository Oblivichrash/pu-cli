// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/context.hpp"
#include "pu/core/fact.hpp"
#include <nlohmann/json.hpp>

using namespace pu::core;

TEST_CASE("Context::Fork creates child with inherited data", "[fork_merge]") {
  auto parent = std::make_shared<Context>("parent");
  parent->Append("user", "Hello");
  parent->Append("assistant", "Hi there!");
  parent->SetVar("theme", nlohmann::json("dark"));
  parent->AddFact(Fact(Fact::Type::kFilePath, "/tmp/file.cpp", "user"));

  auto child = parent->Fork("my-fork");

  REQUIRE(child != nullptr);
  REQUIRE(child->GetParent() == parent);
  REQUIRE(child->GetBranchName() == "my-fork");
  REQUIRE(child->GetState() == Context::State::kActive);

  // Child should inherit vars
  auto theme = child->GetVar("theme");
  REQUIRE(theme.has_value());
  REQUIRE(theme->get<std::string>() == "dark");

  // Child should inherit facts
  REQUIRE(child->GetFacts().size() == 1);
  REQUIRE(child->GetFacts()[0].content == "/tmp/file.cpp");

  // Child should have its own history (fork message only)
  REQUIRE(child->HistorySize() == 1);
  REQUIRE(child->GetHistory()[0].role == "system");
  REQUIRE(child->GetHistory()[0].content.find("Forked") != std::string::npos);

  // Parent should have registered the child
  REQUIRE(parent->GetChildren().size() == 1);
  REQUIRE(parent->GetChildren()[0] == child);
}

TEST_CASE("Context::Fork with empty branch name generates one", "[fork_merge]") {
  auto parent = std::make_shared<Context>("parent");
  auto child = parent->Fork("");

  REQUIRE(child != nullptr);
  REQUIRE(child->GetBranchName().find("fork_") == 0);
}

TEST_CASE("Context::Fork preserves data isolation", "[fork_merge]") {
  auto parent = std::make_shared<Context>("parent");
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

TEST_CASE("Context::Merge creates merge context", "[fork_merge]") {
  auto parent = std::make_shared<Context>("parent");
  parent->Append("user", "Initial question");
  parent->SetVar("config", nlohmann::json("original"));

  auto child = parent->Fork("feature");
  child->Append("assistant", "Research findings");
  child->AddFact(Fact(Fact::Type::kFilePath, "/tmp/result.txt", "tool"));
  child->SetVar("result", nlohmann::json("success"));

  auto merge_ctx = parent->Merge(child, "Task completed successfully");

  REQUIRE(merge_ctx != nullptr);
  REQUIRE(merge_ctx->IsMergeCommit() == true);
  REQUIRE(merge_ctx->GetBranchName() == "main");

  // Merge should contain parent history
  REQUIRE(merge_ctx->HistorySize() >= 2);
  auto history = merge_ctx->GetHistory();
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

  // Merge should contain child's facts
  REQUIRE(merge_ctx->GetFacts().size() > 0);

  // Child should be marked as merged
  REQUIRE(child->GetState() == Context::State::kMerged);

  // Merge parents should be recorded
  auto parents = merge_ctx->GetMergeParents();
  REQUIRE(parents.size() == 2);
}

TEST_CASE("Context::Merge fails on non-child context", "[fork_merge]") {
  auto parent = std::make_shared<Context>("parent");
  auto other = std::make_shared<Context>("other");
  auto child = parent->Fork("valid-child");

  REQUIRE_THROWS_AS(parent->Merge(other, "invalid merge"), std::runtime_error);
  REQUIRE_NOTHROW(parent->Merge(child, "valid merge"));
}

TEST_CASE("Context::Merge fails on already merged child", "[fork_merge]") {
  auto parent = std::make_shared<Context>("parent");
  auto child = parent->Fork("child");
  parent->Merge(child, "first merge");

  REQUIRE_THROWS_AS(parent->Merge(child, "second merge"), std::runtime_error);
}

TEST_CASE("Context::GetTokenCount returns estimate", "[fork_merge]") {
  auto ctx = std::make_shared<Context>("test");
  ctx->Append("user", "Hello world");
  ctx->Append("assistant", "Hi there, how can I help you today?");

  size_t tokens = ctx->GetTokenCount();
  // "Hello world" = 11 chars / 4 = 2.75
  // "Hi there, how can I help you today?" = 37 chars / 4 = 9.25
  // Total: ~12
  REQUIRE(tokens > 0);
  REQUIRE(tokens < 20);
}

TEST_CASE("Fork-Merge tree structure", "[fork_merge]") {
  auto root = std::make_shared<Context>("root");
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
  REQUIRE(child1->GetState() == Context::State::kMerged);
  REQUIRE(child2->GetState() == Context::State::kActive);
  REQUIRE(child3->GetState() == Context::State::kActive);
  REQUIRE(merge1->IsMergeCommit());
}
