// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/summary_generator.hpp"
#include "pu/executor.hpp"

#include <sstream>

namespace pu::core {

SummaryGenerator::SummaryGenerator(agent::AgentManager& manager)
    : manager_(manager) {}

SummaryReport SummaryGenerator::Generate(const std::shared_ptr<Context>& child_ctx,
                                          const Delegation& delegation) {
  SummaryReport report;
  report.status = SummaryReport::Status::kCompleted;
  if (!child_ctx) {
    report.status = SummaryReport::Status::kFailed;
    report.summary = "Child context missing";
    return report;
  }

  std::string prompt = "Summarize the following conversation in 3-5 sentences. "
                       "Focus on key findings and decisions. End with 'DONE'.\n\n";
  auto history = child_ctx->GetHistory();
  for (const auto& msg : history) {
    prompt += msg.role + ": " + msg.content + "\n";
  }

  agent::AgentExecutor executor(manager_);
  auto ctx = executor.PrepareContext(delegation.agent_name, child_ctx);
  std::string summary_text = executor.Execute(delegation.agent_name, prompt, ctx);

  size_t done_pos = summary_text.find("DONE");
  if (done_pos != std::string::npos) {
    summary_text = summary_text.substr(0, done_pos);
  }

  report.summary = summary_text;
  report.key_discoveries = child_ctx->GetFacts();
  return report;
}

}  // namespace pu::core
