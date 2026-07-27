// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/summary_generator.hpp"
#include "pu/executor/executor.hpp"
#include "pu/llm/llm_provider.hpp"
#include "pu/tools/toolbox.hpp"

#include <sstream>

namespace pu {

SummaryGenerator::SummaryGenerator(agent::AgentManager& manager)
    : manager_(manager) {}

HandoffReceipt SummaryGenerator::Generate(const std::shared_ptr<Workspace>& child_ctx,
                                          const Assignment& delegation) {
  HandoffReceipt report;
  report.status = HandoffReceipt::Status::kCompleted;
  if (!child_ctx) {
    report.status = HandoffReceipt::Status::kFailed;
    report.summary = "Child context missing";
    return report;
  }

  std::string prompt = "Summarize the following conversation in 3-5 sentences. "
                       "Focus on key findings and decisions. End with 'DONE'.\n\n";
  auto history = child_ctx->GetHistory();
  for (const auto& msg : history) {
    prompt += msg.role + ": " + msg.content + "\n";
  }

  // TODO: Phase 2 - Use new Executor with proper provider and toolbox
  // For now, return a basic summary
  report.summary = "Summary generation requires Executor with LLMProvider (Phase 2).";
  report.key_discoveries = child_ctx->GetArtifacts();
  return report;
}

}  // namespace pu
