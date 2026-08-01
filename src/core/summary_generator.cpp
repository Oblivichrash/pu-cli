// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/summary_generator.hpp"
#include "pu/executor/executor.hpp"
#include "pu/llm/llm_provider.hpp"
#include "pu/tools/toolbox.hpp"

#include <sstream>

namespace pu {

SummaryGenerator::SummaryGenerator(AgentManager& manager)
    : manager_(manager) {}

HandoffReceipt SummaryGenerator::Generate(const std::shared_ptr<Workspace>& child_workspace,
                                          const Assignment& delegation,
                                          LLMProvider* provider) {
  (void)delegation;  // Unused; kept for interface compatibility

  HandoffReceipt report;
  report.status = HandoffReceipt::Status::kCompleted;
  if (!child_workspace) {
    report.status = HandoffReceipt::Status::kFailed;
    report.summary = "Child workspace missing";
    return report;
  }

  std::string prompt = "Summarize the following conversation in 3-5 sentences. "
                       "Focus on key findings, decisions, and unresolved issues.\n\n---\n";
  auto history = child_workspace->GetHistory();
  for (const auto& msg : history) {
    prompt += msg.role + ": " + msg.content + "\n";
  }
  prompt += "\n---\nSummary:\n";

  if (provider) {
    std::string summary_text;
    auto content_callback = [&summary_text](const std::string& chunk) {
      summary_text += chunk;
    };

    try {
      std::vector<ChatMessage> msg_history;
      ChatMessage system_msg;
      system_msg.role = "system";
      system_msg.content = "You are a helpful assistant that summarizes conversations.";
      msg_history.push_back(system_msg);

      ChatMessage user_msg;
      user_msg.role = "user";
      user_msg.content = prompt;
      msg_history.push_back(user_msg);

      auto result = provider->Chat(msg_history, {}, content_callback, nullptr);

      if (!summary_text.empty()) {
        report.summary = summary_text;
      } else if (!result.content.empty()) {
        report.summary = result.content;
      } else {
        report.summary = "[Summary generation returned empty result]";
      }
    } catch (const std::exception& e) {
      report.summary = "[Summary generation failed: " + std::string(e.what()) + "]";
    }
  } else {
    report.summary = "Summary generation requires an LLM provider. "
                     "The conversation had " + std::to_string(history.size()) + " messages.";
  }

  report.key_discoveries = child_workspace->GetArtifacts();
  return report;
}

}  // namespace pu
