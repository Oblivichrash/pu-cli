// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/summary_generator.hpp"
#include "pu/session/workspace.hpp"
#include "pu/session/assignment.hpp"
#include "pu/agent/agent_manager.hpp"

using namespace pu;

// Mock AgentManager - minimal implementation for testing
namespace {

class TestAgentManager : public pu::AgentManager {
 public:
  TestAgentManager() : pu::AgentManager() {}
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

TEST_CASE("SummaryGenerator returns completed report for valid workspace", "[summary_generator]") {
  TestAgentManager manager;
  SummaryGenerator generator(manager);

  auto child_ctx = std::make_shared<Workspace>("test-ctx");
  child_ctx->Append("user", "Hello");
  child_ctx->Append("assistant", "Hi there!");

  Assignment delegation;
  delegation.goal = "test goal";
  delegation.agent_name = "test-agent";
  auto report = generator.Generate(child_ctx, delegation);

  REQUIRE(report.status == HandoffReceipt::kCompleted);
  REQUIRE_FALSE(report.summary.empty());
}

TEST_CASE("SummaryGenerator extracts artifacts from workspace", "[summary_generator]") {
  TestAgentManager manager;
  SummaryGenerator generator(manager);

  auto child_ctx = std::make_shared<Workspace>("test-ctx");
  child_ctx->Append("user", "Create a file");
  child_ctx->Append("assistant", "Created file.txt");
  
  Artifact artifact;
  artifact.type = Artifact::Type::kFilePath;
  artifact.content = "/tmp/file.txt";
  artifact.source = "assistant";
  child_ctx->AddArtifact(artifact);

  Assignment delegation;
  delegation.goal = "create file";
  delegation.agent_name = "test-agent";
  auto report = generator.Generate(child_ctx, delegation);

  REQUIRE(report.status == HandoffReceipt::kCompleted);
  REQUIRE(report.key_discoveries.size() == 1);
  REQUIRE(report.key_discoveries[0].type == Artifact::Type::kFilePath);
  REQUIRE(report.key_discoveries[0].content == "/tmp/file.txt");
}
