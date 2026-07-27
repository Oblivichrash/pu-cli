// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/summary_generator.hpp"
#include "pu/core/context.hpp"
#include "pu/core/delegation.hpp"
#include "pu/agent_core.hpp"

using namespace pu::core;

// Mock AgentManager - minimal implementation for testing
namespace {

class TestAgentManager : public pu::agent::AgentManager {
 public:
  TestAgentManager() : pu::agent::AgentManager() {}
};

}  // namespace

TEST_CASE("SummaryGenerator returns failed report for null context", "[summary_generator]") {
  TestAgentManager manager;
  SummaryGenerator generator(manager);

  Delegation delegation("test goal", "test-agent", {}, 0);
  auto report = generator.Generate(nullptr, delegation);

  REQUIRE(report.status == SummaryReport::Status::kFailed);
  REQUIRE(report.summary == "Child context missing");
}

TEST_CASE("SummaryGenerator handles empty context", "[summary_generator]") {
  auto ctx = std::make_shared<Context>("test");
  TestAgentManager manager;
  SummaryGenerator generator(manager);

  Delegation delegation("test goal", "test-agent", {}, 0);
  auto report = generator.Generate(ctx, delegation);

  // With no LLM backend available, the summary generation will fail
  // But the function should not crash and return a report
  bool is_valid = (report.status == SummaryReport::Status::kCompleted) ||
                  (report.status == SummaryReport::Status::kFailed);
  REQUIRE(is_valid);
}

TEST_CASE("SummaryGenerator produces report with facts from context", "[summary_generator]") {
  auto ctx = std::make_shared<Context>("test");
  ctx->Append("user", "Initial question");
  ctx->AddFact(Fact(Fact::Type::kFilePath, "/tmp/test.txt", "user"));

  TestAgentManager manager;
  SummaryGenerator generator(manager);

  Delegation delegation("test goal", "test-agent", {}, 0);
  auto report = generator.Generate(ctx, delegation);

  // Facts from context should be passed through
  REQUIRE(report.key_discoveries.size() == 1);
  REQUIRE(report.key_discoveries[0].content == "/tmp/test.txt");
}
