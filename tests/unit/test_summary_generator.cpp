// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/summary_generator.hpp"
#include "pu/session/workspace.hpp"
#include "pu/session/assignment.hpp"
#include "pu/agent_core.hpp"

using namespace pu;

// Mock AgentManager - minimal implementation for testing
namespace {

class TestAgentManager : public pu::agent::AgentManager {
 public:
  TestAgentManager() : pu::agent::AgentManager() {}
};

}  // namespace

TEST_CASE("SummaryGenerator returns failed report for null workspace", "[summary_generator]") {
  TestAgentManager manager;
  SummaryGenerator generator(manager);

  Assignment delegation;
  delegation.goal = "test goal";
  delegation.agent_name = "test-agent";
  auto report = generator.Generate(nullptr, delegation);

  REQUIRE(report.status == HandoffReceipt::kFailed);
  REQUIRE(report.summary == "Child context missing");
}

TEST_CASE("SummaryGenerator handles empty workspace", "[summary_generator]") {
  auto ctx = std::make_shared<Workspace>("test");
  TestAgentManager manager;
  SummaryGenerator generator(manager);

  Assignment delegation;
  delegation.goal = "test goal";
  delegation.agent_name = "test-agent";
  auto report = generator.Generate(ctx, delegation);

  // With no LLM backend available, the summary generation will fail
  // But the function should not crash and return a report
  bool is_valid = (report.status == HandoffReceipt::kCompleted) ||
                  (report.status == HandoffReceipt::kFailed);
  REQUIRE(is_valid);
}

TEST_CASE("SummaryGenerator produces report with artifacts from workspace", "[summary_generator]") {
  auto ctx = std::make_shared<Workspace>("test");
  ctx->Append("user", "Initial question");
  Artifact a;
  a.type = Artifact::Type::kFilePath;
  a.content = "/tmp/test.txt";
  a.source = "user";
  ctx->AddArtifact(a);

  TestAgentManager manager;
  SummaryGenerator generator(manager);

  Assignment delegation;
  delegation.goal = "test goal";
  delegation.agent_name = "test-agent";
  auto report = generator.Generate(ctx, delegation);

  // Artifacts from workspace should be passed through
  REQUIRE(report.key_discoveries.size() == 1);
  REQUIRE(report.key_discoveries[0].content == "/tmp/test.txt");
}
