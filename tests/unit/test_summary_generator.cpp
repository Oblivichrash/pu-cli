// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/summary_generator.hpp"
#include "pu/session/workspace.hpp"
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

  auto report = generator.Generate(nullptr);

  REQUIRE(report.status == SummaryGenerator::SummaryResult::kFailed);
  REQUIRE(report.summary == "Child workspace missing");
}

TEST_CASE("SummaryGenerator returns completed report for valid workspace", "[summary_generator]") {
  TestAgentManager manager;
  SummaryGenerator generator(manager);

  auto child_workspace = std::make_shared<Workspace>("test-ws");
  child_workspace->Append("user", "Hello");
  child_workspace->Append("assistant", "Hi there!");

  auto report = generator.Generate(child_workspace);

  REQUIRE(report.status == SummaryGenerator::SummaryResult::kCompleted);
  REQUIRE_FALSE(report.summary.empty());
}

TEST_CASE("SummaryGenerator extracts artifacts from workspace", "[summary_generator]") {
  TestAgentManager manager;
  SummaryGenerator generator(manager);

  auto child_workspace = std::make_shared<Workspace>("test-ws");
  child_workspace->Append("user", "Create a file");
  child_workspace->Append("assistant", "Created file.txt");

  Artifact artifact;
  artifact.type = Artifact::Type::kFilePath;
  artifact.content = "/tmp/file.txt";
  artifact.source = "assistant";
  child_workspace->AddArtifact(artifact);

  auto report = generator.Generate(child_workspace);

  REQUIRE(report.status == SummaryGenerator::SummaryResult::kCompleted);
  REQUIRE(report.key_discoveries.size() == 1);
  REQUIRE(report.key_discoveries[0].type == Artifact::Type::kFilePath);
  REQUIRE(report.key_discoveries[0].content == "/tmp/file.txt");
}
