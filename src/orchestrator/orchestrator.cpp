// SPDX-License-Identifier: GPL-3.0-only
#include "pu/orchestrator.hpp"
#include "pu/executor.hpp"

#include <algorithm>
#include <chrono>
#include <regex>
#include <sstream>

namespace pu {

Orchestrator::Orchestrator(std::shared_ptr<GlobalContext> ctx,
                           std::shared_ptr<CallStack> stack,
                           agent::AgentManager& manager)
    : ctx_(std::move(ctx)), stack_(std::move(stack)), manager_(manager) {}

void Orchestrator::SetDelegationStack(std::shared_ptr<core::DelegationStack> stack) {
  delegation_stack_ = std::move(stack);
  if (delegation_stack_) {
    root_context_ = delegation_stack_->GetRootContext();
  }
}

core::FactList Orchestrator::ExtractFacts(const std::shared_ptr<core::Context>& ctx,
                                          const std::string& goal) {
  core::FactList facts;
  if (!ctx) return facts;
  (void)goal;

  auto history = ctx->Recent(20);
  for (const auto& msg : history) {
    const std::string& text = msg.content;

    static std::regex file_re(R"((/[^\s]+\.\w+)|([a-zA-Z0-9_\-\.]+\.[a-zA-Z0-9]+))");
    std::smatch match;
    if (std::regex_search(text, match, file_re)) {
      facts.emplace_back(core::Fact::Type::kFilePath, match.str(), msg.role);
    }

    if (text.find("error") != std::string::npos || text.find("fail") != std::string::npos) {
      facts.emplace_back(core::Fact::Type::kErrorMsg, text.substr(0, 200), msg.role);
    }
  }

  std::sort(facts.begin(), facts.end(),
            [](const core::Fact& a, const core::Fact& b) { return a.content < b.content; });
  facts.erase(std::unique(facts.begin(), facts.end(),
                          [](const core::Fact& a, const core::Fact& b) {
                            return a.content == b.content;
                          }),
              facts.end());

  return facts;
}

core::SummaryReport Orchestrator::GenerateSummary(
    const std::shared_ptr<core::Context>& child_ctx,
    const core::Delegation& delegation) {
  core::SummaryReport report;
  report.status = core::SummaryReport::Status::kCompleted;

  if (!child_ctx) {
    report.status = core::SummaryReport::Status::kFailed;
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

void Orchestrator::InjectSummaryIntoParent(const core::SummaryReport& report) {
  if (delegation_stack_ && !delegation_stack_->IsEmpty()) {
    auto parent_ctx = delegation_stack_->CurrentContext();
    if (parent_ctx) {
      parent_ctx->Append("system", "[Sub-task] " + report.summary);
      for (const auto& f : report.key_discoveries) {
        parent_ctx->AddFact(f);
      }
    }
  } else if (root_context_) {
    root_context_->Append("system", "[Completed delegation] " + report.summary);
  }
}

bool Orchestrator::HandleCommand(const std::string& input, std::string& output) {
  if (input.rfind("/push ", 0) == 0) {
    std::string args = input.substr(6);
    size_t space = args.find(' ');
    if (space == std::string::npos) {
      output = "Usage: /push <agent> \"<goal>\"";
      return true;
    }
    std::string agent_name = args.substr(0, space);
    std::string goal = args.substr(space + 1);

    if (goal.size() >= 2 && goal.front() == '"' && goal.back() == '"') {
      goal = goal.substr(1, goal.size() - 2);
    }
    if (goal.empty()) {
      output = "Error: goal cannot be empty";
      return true;
    }

    int current_depth = delegation_stack_ ? static_cast<int>(delegation_stack_->Depth()) : 0;
    if (current_depth >= max_depth_) {
      output = "Error: delegation depth limit reached (" + std::to_string(max_depth_) + ")";
      return true;
    }

    auto facts = ExtractFacts(root_context_, goal);

    core::Delegation deleg(goal, agent_name, facts, current_depth);
    deleg.id = core::Delegation::GenerateId();
    deleg.created_at = std::chrono::steady_clock::now();
    deleg.deadline = deleg.created_at + std::chrono::seconds(60);

    auto child_ctx = std::make_shared<core::Context>("ctx-" + deleg.id);
    child_ctx->AddFacts(facts);
    child_ctx->Append("system", "Delegation started: " + goal);

    delegation_stack_->Push(deleg, child_ctx);
    output = "Pushed agent: " + agent_name + " with goal: " + goal;
    return true;
  }

  if (input == "/pop") {
    if (!delegation_stack_ || delegation_stack_->IsEmpty()) {
      output = "Error: no active delegation to pop";
      return true;
    }

    auto report = PopDelegation();
    InjectSummaryIntoParent(report);
    output = "Popped delegation: " + report.summary;
    return true;
  }

  if (input == "/stack") {
    if (delegation_stack_ && !delegation_stack_->IsEmpty()) {
      std::ostringstream oss;
      oss << "Delegation stack (depth " << delegation_stack_->Depth() << "):\n";
      for (const auto& frame : delegation_stack_->GetFrames()) {
        oss << "  " << frame.delegation.agent_name << " [" << frame.delegation.id << "]\n";
      }
      output = oss.str();
    } else if (delegation_stack_) {
      output = "Delegation stack is empty (depth 0)";
    } else {
      output = "No delegation stack active";
    }
    return true;
  }

  return false;
}

std::string Orchestrator::Process(const std::string& input) {
  std::string current_input = input;
  std::string final_response;
  agent::AgentExecutor executor(manager_);

  if (root_context_) {
    executor.SetRootContext(root_context_);
  }

  if (delegation_stack_ && !delegation_stack_->IsEmpty()) {
    const auto& frame = delegation_stack_->Current();
    std::string agent_name = frame.delegation.agent_name;
    auto child_ctx = frame.context;

    agent::AgentContext ctx = executor.PrepareContext(agent_name, child_ctx);
    std::string response = executor.Execute(agent_name, current_input, ctx);

    if (ctx.pending_action.type == agent::PendingAction::Type::kPush) {
      // Phase 4: automatic delegation
    }

    if (ctx.pending_action.type == agent::PendingAction::Type::kPop) {
      if (!delegation_stack_->IsEmpty()) {
        auto report = PopDelegation();
        InjectSummaryIntoParent(report);
      }

      if (delegation_stack_->IsEmpty()) {
        final_response = response;
      } else {
        current_input = response;
        return Process(current_input);
      }
    }

    final_response = response;
  } else {
    while (true) {
      if (stack_->IsEmpty()) {
        final_response = executor.Dispatch(current_input);
        break;
      }

      const StackFrame& top = stack_->Top();
      std::string agent_name = top.agent_name;

      agent::AgentContext ctx = executor.PrepareContext(agent_name);
      std::string response = executor.Execute(agent_name, current_input, ctx);

      if (ctx.pending_action.type == agent::PendingAction::Type::kPush) {
        Push(ctx.pending_action.agent_name);
        current_input = "";
        continue;
      }

      if (ctx.pending_action.type == agent::PendingAction::Type::kPop) {
        if (!stack_->IsEmpty()) {
          Pop();
        }
        if (stack_->IsEmpty()) {
          final_response = response;
          break;
        } else {
          current_input = response;
          continue;
        }
      }

      final_response = response;
      break;
    }
  }

  return final_response;
}

void Orchestrator::Push(const std::string& agent_name) {
  StackFrame frame;
  frame.agent_name = agent_name;
  frame.invocation_id = "manual_" + std::to_string(stack_->Size() + 1);
  stack_->Push(frame);
}

void Orchestrator::Pop() {
  if (!stack_->IsEmpty()) {
    stack_->Pop();
  }
}

std::string Orchestrator::ShowStack() const {
  if (stack_->IsEmpty()) {
    return "Stack is empty";
  }
  std::ostringstream oss;
  oss << "Stack (top to bottom):\n";
  const auto& frames = stack_->GetFrames();
  for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
    oss << "  " << it->agent_name << " (" << it->invocation_id << ")\n";
  }
  return oss.str();
}

bool Orchestrator::PushDelegation(const std::string& agent_name, const std::string& goal) {
  if (!delegation_stack_) return false;

  core::Delegation deleg(goal, agent_name, {}, static_cast<int>(delegation_stack_->Depth()));
  deleg.id = core::Delegation::GenerateId();
  deleg.created_at = std::chrono::steady_clock::now();
  deleg.deadline = deleg.created_at + std::chrono::seconds(30);

  delegation_stack_->Push(deleg);
  return true;
}

core::SummaryReport Orchestrator::PopDelegation() {
  if (!delegation_stack_ || delegation_stack_->IsEmpty()) {
    core::SummaryReport report;
    report.status = core::SummaryReport::Status::kFailed;
    report.summary = "No active delegation to pop";
    return report;
  }

  auto& frame = delegation_stack_->Current();
  auto report = GenerateSummary(frame.context, frame.delegation);
  frame.delegation.result = report;

  delegation_stack_->Pop();

  return report;
}

}  // namespace pu
