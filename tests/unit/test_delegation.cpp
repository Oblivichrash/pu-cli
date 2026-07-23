// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/delegation.hpp"
#include "pu/core/delegation_stack.hpp"
#include "pu/core/context.hpp"
#include "pu/core/fact.hpp"

using namespace pu::core;

TEST_CASE("DelegationStack push and pop", "[delegation]") {
  auto root = std::make_shared<Context>("root");
  DelegationStack stack(root);

  REQUIRE(stack.IsEmpty());
  REQUIRE(stack.Depth() == 0);

  Delegation d1("goal1", "agent1", {}, 0);
  d1.id = Delegation::GenerateId();
  auto ctx1 = std::make_shared<Context>("ctx1");
  stack.Push(d1, ctx1);

  REQUIRE(!stack.IsEmpty());
  REQUIRE(stack.Depth() == 1);
  REQUIRE(stack.Current().delegation.goal == "goal1");
  REQUIRE(stack.CurrentContext() == ctx1);

  Delegation d2("goal2", "agent2", {}, 1);
  d2.id = Delegation::GenerateId();
  auto ctx2 = std::make_shared<Context>("ctx2");
  stack.Push(d2, ctx2);

  REQUIRE(stack.Depth() == 2);
  REQUIRE(stack.Current().delegation.agent_name == "agent2");

  auto report = stack.Pop();
  REQUIRE(stack.Depth() == 1);
  REQUIRE(report.status == SummaryReport::Status::kCompleted);

  stack.Pop();
  REQUIRE(stack.IsEmpty());
}

TEST_CASE("DelegationStack push without explicit context creates one", "[delegation]") {
  auto root = std::make_shared<Context>("root");
  DelegationStack stack(root);

  Delegation d("goal", "agent", {}, 0);
  d.id = Delegation::GenerateId();
  stack.Push(d);

  REQUIRE(stack.Depth() == 1);
  REQUIRE(stack.CurrentContext() != nullptr);
  REQUIRE(stack.CurrentContext() != root);
}

TEST_CASE("DelegationStack pop on empty throws", "[delegation]") {
  auto root = std::make_shared<Context>("root");
  DelegationStack stack(root);

  REQUIRE_THROWS_AS(stack.Pop(), std::runtime_error);
}

TEST_CASE("DelegationStack Current on empty throws", "[delegation]") {
  auto root = std::make_shared<Context>("root");
  DelegationStack stack(root);

  REQUIRE_THROWS_AS(stack.Current(), std::runtime_error);
  REQUIRE_THROWS_AS(stack.CurrentContext(), std::runtime_error);
}

TEST_CASE("DelegationStack Clear", "[delegation]") {
  auto root = std::make_shared<Context>("root");
  DelegationStack stack(root);

  Delegation d("goal", "agent", {}, 0);
  d.id = Delegation::GenerateId();
  stack.Push(d);
  stack.Push(d);
  REQUIRE(stack.Depth() == 2);

  stack.Clear();
  REQUIRE(stack.IsEmpty());
}

TEST_CASE("DelegationStack GetFrames", "[delegation]") {
  auto root = std::make_shared<Context>("root");
  DelegationStack stack(root);

  Delegation d1("goal1", "agent1", {}, 0);
  d1.id = Delegation::GenerateId();
  stack.Push(d1);

  Delegation d2("goal2", "agent2", {}, 1);
  d2.id = Delegation::GenerateId();
  stack.Push(d2);

  const auto& frames = stack.GetFrames();
  REQUIRE(frames.size() == 2);
  REQUIRE(frames[0].delegation.goal == "goal1");
  REQUIRE(frames[1].delegation.goal == "goal2");
}

TEST_CASE("Delegation GenerateId produces unique IDs", "[delegation]") {
  std::string id1 = Delegation::GenerateId();
  std::string id2 = Delegation::GenerateId();
  REQUIRE(id1 != id2);
  REQUIRE(id1.substr(0, 5) == "deleg");
}

TEST_CASE("Delegation IsTimeout", "[delegation]") {
  Delegation d;
  d.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  REQUIRE(d.IsTimeout());

  Delegation d2;
  d2.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  REQUIRE(!d2.IsTimeout());

  Delegation d3;
  REQUIRE(!d3.IsTimeout());
}

TEST_CASE("DelegationStack pop with explicit result", "[delegation]") {
  auto root = std::make_shared<Context>("root");
  DelegationStack stack(root);

  Delegation d("goal", "agent", {}, 0);
  d.id = Delegation::GenerateId();
  auto ctx = std::make_shared<Context>("ctx");
  stack.Push(d, ctx);

  SummaryReport expected;
  expected.status = SummaryReport::Status::kCompleted;
  expected.summary = "Task completed successfully";
  stack.Current().delegation.result = expected;

  auto report = stack.Pop();
  REQUIRE(report.status == SummaryReport::Status::kCompleted);
  REQUIRE(report.summary == "Task completed successfully");
}

TEST_CASE("DelegationStack pop without explicit result gets default", "[delegation]") {
  auto root = std::make_shared<Context>("root");
  DelegationStack stack(root);

  Delegation d("goal", "agent", {}, 0);
  d.id = Delegation::GenerateId();
  auto ctx = std::make_shared<Context>("ctx");
  stack.Push(d, ctx);

  auto report = stack.Pop();
  REQUIRE(report.status == SummaryReport::Status::kCompleted);
  REQUIRE(!report.summary.empty());
}

TEST_CASE("SummaryReport IsSuccess and IsFinal", "[delegation]") {
  SummaryReport r;
  r.status = SummaryReport::Status::kCompleted;
  r.summary = "done";
  REQUIRE(r.IsSuccess());
  REQUIRE(r.IsFinal());

  SummaryReport r2;
  r2.status = SummaryReport::Status::kFailed;
  REQUIRE(!r2.IsSuccess());
  REQUIRE(r2.IsFinal());

  SummaryReport r3;
  r3.status = SummaryReport::Status::kCompleted;
  r3.summary = "";
  REQUIRE(r3.IsSuccess());
  REQUIRE(!r3.IsFinal());
}

TEST_CASE("Delegation with seeded facts", "[delegation]") {
  FactList facts;
  facts.emplace_back(Fact::Type::kFilePath, "/tmp/test.cpp", "user");
  facts.emplace_back(Fact::Type::kErrorMsg, "error: something failed", "system");

  Delegation d("review code", "code-agent", facts, 0);
  REQUIRE(d.seeded_facts.size() == 2);
  REQUIRE(d.seeded_facts[0].content == "/tmp/test.cpp");
  REQUIRE(d.depth == 0);
}

TEST_CASE("Context isolation between parent and child", "[delegation]") {
  auto parent = std::make_shared<Context>("parent");
  parent->Append("user", "Hello from parent");
  parent->AddFact(Fact(Fact::Type::kFilePath, "/tmp/parent.cpp", "user"));

  auto child = std::make_shared<Context>("child");
  FactList seed_facts;
  seed_facts.push_back(parent->GetFacts()[0]);
  child->AddFacts(seed_facts);
  child->Append("system", "Delegation started");

  REQUIRE(child->GetFacts().size() == 1);
  REQUIRE(child->GetFacts()[0].content == "/tmp/parent.cpp");
  REQUIRE(child->HistorySize() == 1);
  REQUIRE(child->GetHistory()[0].role == "system");

  REQUIRE(parent->HistorySize() == 1);
  REQUIRE(parent->GetFacts().size() == 1);

  child->Append("assistant", "Working on it");
  REQUIRE(child->HistorySize() == 2);
  REQUIRE(parent->HistorySize() == 1);
}

TEST_CASE("DelegationStack depth tracking", "[delegation]") {
  auto root = std::make_shared<Context>("root");
  DelegationStack stack(root);

  for (int i = 0; i < 5; ++i) {
    Delegation d("goal" + std::to_string(i), "agent", {}, i);
    d.id = Delegation::GenerateId();
    stack.Push(d);
    REQUIRE(stack.Depth() == static_cast<size_t>(i + 1));
    REQUIRE(stack.Current().delegation.depth == i);
  }

  for (int i = 4; i >= 0; --i) {
    REQUIRE(stack.Depth() == static_cast<size_t>(i + 1));
    stack.Pop();
  }
  REQUIRE(stack.IsEmpty());
}

TEST_CASE("DelegationStack GetRootContext", "[delegation]") {
  auto root = std::make_shared<Context>("root");
  DelegationStack stack(root);

  REQUIRE(stack.GetRootContext() == root);

  Delegation d("goal", "agent", {}, 0);
  d.id = Delegation::GenerateId();
  stack.Push(d);

  REQUIRE(stack.GetRootContext() == root);
  REQUIRE(stack.CurrentContext() != root);
}
