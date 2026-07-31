// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/session/assignment.hpp"
#include "pu/session/call_stack.hpp"
#include "pu/session/workspace.hpp"
#include "pu/session/artifact.hpp"
#include "pu/agent/agent_manager.hpp"

using namespace pu;

namespace {

class MockAgentManager : public pu::AgentManager {
 public:
  MockAgentManager() : pu::AgentManager() {}
};

}  // namespace

TEST_CASE("CallStack push and pop", "[delegation]") {
  auto root = std::make_shared<Workspace>("root");
  auto stack = CallStack::Create(root);

  REQUIRE(stack->IsEmpty());
  REQUIRE(stack->Depth() == 0);

  Assignment d1;
  d1.goal = "goal1";
  d1.agent_name = "agent1";
  d1.depth = 0;
  d1.id = Assignment::GenerateId();
  auto ctx1 = std::make_shared<Workspace>("ctx1");
  stack->Push(d1, ctx1);

  REQUIRE(!stack->IsEmpty());
  REQUIRE(stack->Depth() == 1);
  REQUIRE(stack->Current().assignment.goal == "goal1");
  REQUIRE(stack->CurrentContext() == ctx1);

  Assignment d2;
  d2.goal = "goal2";
  d2.agent_name = "agent2";
  d2.depth = 1;
  d2.id = Assignment::GenerateId();
  auto ctx2 = std::make_shared<Workspace>("ctx2");
  stack->Push(d2, ctx2);

  REQUIRE(stack->Depth() == 2);
  REQUIRE(stack->Current().assignment.agent_name == "agent2");

  auto report = stack->Pop();
  REQUIRE(report.has_value());
  REQUIRE(stack->Depth() == 1);

  stack->Pop();
  REQUIRE(stack->IsEmpty());
}

TEST_CASE("CallStack push without explicit context creates one", "[delegation]") {
  auto root = std::make_shared<Workspace>("root");
  auto stack = CallStack::Create(root);

  Assignment d;
  d.goal = "goal";
  d.agent_name = "agent";
  d.depth = 0;
  d.id = Assignment::GenerateId();
  stack->Push(d);

  REQUIRE(stack->Depth() == 1);
  REQUIRE(stack->CurrentContext() != nullptr);
  REQUIRE(stack->CurrentContext() != root);
}

TEST_CASE("CallStack Current on empty throws", "[delegation]") {
  auto root = std::make_shared<Workspace>("root");
  auto stack = CallStack::Create(root);

  REQUIRE_THROWS_AS(stack->Current(), std::runtime_error);
  REQUIRE_THROWS_AS(stack->CurrentContext(), std::runtime_error);
}

TEST_CASE("CallStack Clear", "[delegation]") {
  auto root = std::make_shared<Workspace>("root");
  auto stack = CallStack::Create(root);

  Assignment d;
  d.goal = "goal";
  d.agent_name = "agent";
  d.id = Assignment::GenerateId();
  stack->Push(d);
  stack->Push(d);
  REQUIRE(stack->Depth() == 2);

  stack->Clear();
  REQUIRE(stack->IsEmpty());
}

TEST_CASE("CallStack GetFrames", "[delegation]") {
  auto root = std::make_shared<Workspace>("root");
  auto stack = CallStack::Create(root);

  Assignment d1;
  d1.goal = "goal1";
  d1.agent_name = "agent1";
  d1.id = Assignment::GenerateId();
  stack->Push(d1);

  Assignment d2;
  d2.goal = "goal2";
  d2.agent_name = "agent2";
  d2.id = Assignment::GenerateId();
  stack->Push(d2);

  const auto& frames = stack->GetFrames();
  REQUIRE(frames.size() == 2);
  REQUIRE(frames[0].assignment.goal == "goal1");
  REQUIRE(frames[1].assignment.goal == "goal2");
}

TEST_CASE("Assignment GenerateId produces unique IDs", "[delegation]") {
  std::string id1 = Assignment::GenerateId();
  std::string id2 = Assignment::GenerateId();
  REQUIRE(id1 != id2);
  REQUIRE(id1.substr(0, 4) == "asgn");
}

TEST_CASE("Assignment IsTimeout", "[delegation]") {
  Assignment d;
  d.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  REQUIRE(d.IsTimeout());

  Assignment d2;
  d2.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  REQUIRE(!d2.IsTimeout());

  Assignment d3;
  REQUIRE(!d3.IsTimeout());
}

TEST_CASE("CallStack pop with explicit result", "[delegation]") {
  auto root = std::make_shared<Workspace>("root");
  auto stack = CallStack::Create(root);

  Assignment d;
  d.goal = "goal";
  d.agent_name = "agent";
  d.id = Assignment::GenerateId();
  auto ctx = std::make_shared<Workspace>("ctx");
  stack->Push(d, ctx);

  HandoffReceipt expected;
  expected.status = HandoffReceipt::kCompleted;
  expected.summary = "Task completed successfully";
  stack->Current().assignment.result = expected;

  auto report = stack->Pop();
  REQUIRE(report.has_value());
  REQUIRE(report->status == HandoffReceipt::kCompleted);
  REQUIRE(report->summary == "Task completed successfully");
}

TEST_CASE("CallStack pop without explicit result gets default", "[delegation]") {
  auto root = std::make_shared<Workspace>("root");
  auto stack = CallStack::Create(root);

  Assignment d;
  d.goal = "goal";
  d.agent_name = "agent";
  d.id = Assignment::GenerateId();
  auto ctx = std::make_shared<Workspace>("ctx");
  stack->Push(d, ctx);

  auto report = stack->Pop();
  REQUIRE(report.has_value());
  REQUIRE(report->status == HandoffReceipt::kCompleted);
  REQUIRE(!report->summary.empty());
}

TEST_CASE("Assignment with seeded artifacts", "[delegation]") {
  std::vector<Artifact> artifacts;
  Artifact a1;
  a1.type = Artifact::Type::kFilePath;
  a1.content = "/tmp/test.cpp";
  a1.source = "user";
  artifacts.push_back(a1);
  Artifact a2;
  a2.type = Artifact::Type::kErrorMsg;
  a2.content = "error: something failed";
  a2.source = "system";
  artifacts.push_back(a2);

  Assignment d;
  d.goal = "review code";
  d.agent_name = "code-agent";
  d.seeded_artifacts = artifacts;
  d.depth = 0;

  REQUIRE(d.seeded_artifacts.size() == 2);
  REQUIRE(d.seeded_artifacts[0].content == "/tmp/test.cpp");
  REQUIRE(d.depth == 0);
}

TEST_CASE("Context isolation between parent and child", "[delegation]") {
  auto parent = std::make_shared<Workspace>("parent");
  parent->Append("user", "Hello from parent");
  Artifact a;
  a.type = Artifact::Type::kFilePath;
  a.content = "/tmp/parent.cpp";
  a.source = "user";
  parent->AddArtifact(a);

  auto child = std::make_shared<Workspace>("child");
  std::vector<Artifact> seed_artifacts;
  seed_artifacts.push_back(parent->GetArtifacts()[0]);
  for (const auto& art : seed_artifacts) {
    child->AddArtifact(art);
  }
  child->Append("system", "Delegation started");

  REQUIRE(child->GetArtifacts().size() == 1);
  REQUIRE(child->GetArtifacts()[0].content == "/tmp/parent.cpp");
  REQUIRE(child->HistorySize() == 1);
  REQUIRE(child->GetHistory()[0].role == "system");

  REQUIRE(parent->HistorySize() == 1);
  REQUIRE(parent->GetArtifacts().size() == 1);

  child->Append("assistant", "Working on it");
  REQUIRE(child->HistorySize() == 2);
  REQUIRE(parent->HistorySize() == 1);
}

TEST_CASE("CallStack depth tracking", "[delegation]") {
  auto root = std::make_shared<Workspace>("root");
  auto stack = CallStack::Create(root);

  for (int i = 0; i < 5; ++i) {
    Assignment d;
    d.goal = "goal" + std::to_string(i);
    d.agent_name = "agent";
    d.depth = i;
    d.id = Assignment::GenerateId();
    stack->Push(d);
    REQUIRE(stack->Depth() == static_cast<size_t>(i + 1));
    REQUIRE(stack->Current().assignment.depth == i);
  }

  for (int i = 4; i >= 0; --i) {
    REQUIRE(stack->Depth() == static_cast<size_t>(i + 1));
    stack->Pop();
  }
  REQUIRE(stack->IsEmpty());
}

TEST_CASE("CallStack GetRootContext", "[delegation]") {
  auto root = std::make_shared<Workspace>("root");
  auto stack = CallStack::Create(root);

  REQUIRE(stack->GetRootContext() == root);

  Assignment d;
  d.goal = "goal";
  d.agent_name = "agent";
  d.id = Assignment::GenerateId();
  stack->Push(d);

  REQUIRE(stack->GetRootContext() == root);
  REQUIRE(stack->CurrentContext() != root);
}