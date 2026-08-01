// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/fork_tools.hpp"
#include "pu/session/call_stack.hpp"
#include "pu/session/workspace.hpp"
#include "pu/core/fork_merge_service.hpp"

#include <nlohmann/json.hpp>

#include <sstream>

namespace pu::tools {

std::string ForkContextTool::Name() const {
  return "fork_context";
}

std::string ForkContextTool::Description() const {
  return "Fork a new isolated context for sub-task execution. "
         "The child context inherits all data from the parent but "
         "has independent history. Use for complex tasks that require "
         "multiple tool calls or deep exploration.";
}

std::string ForkContextTool::ParametersSchema() const {
  return R"schema({
    "type": "object",
    "properties": {
      "agent_name": {
        "type": "string",
        "description": "Agent to execute the sub-task"
      },
      "goal": {
        "type": "string",
        "description": "Clear description of the sub-task goal"
      },
      "branch_name": {
        "type": "string",
        "description": "Optional branch name (auto-generated if omitted)"
      }
    },
    "required": ["agent_name", "goal"]
  })schema";
}

std::string ForkContextTool::Execute(const nlohmann::json& args, pu::ToolContext& ctx) {
  std::string agent_name = args.value("agent_name", "");
  std::string goal = args.value("goal", "");
  std::string branch_name = args.value("branch_name", "");

  if (agent_name.empty()) {
    return "Error: 'agent_name' is required";
  }
  if (goal.empty()) {
    return "Error: 'goal' is required";
  }

  if (!ctx.fork_service) {
    return "Error: ForkMergeService not available in this context. "
           "Please use /fork command instead.";
  }
  if (!ctx.call_stack) {
    return "Error: CallStack not available in this context.";
  }

  auto parent = ctx.call_stack->IsEmpty() ? ctx.fork_service->GetRootContext()
                                         : ctx.call_stack->CurrentContext();
  auto result = ctx.fork_service->Fork(parent, agent_name, goal, branch_name);
  if (!result.child_context) {
    return "Error: " + result.message;
  }

  Assignment asgn;
  asgn.goal = "exploration";
  asgn.agent_name = agent_name;
  ctx.call_stack->Push(asgn, result.child_context);

  return result.message;
}

std::string MergeContextTool::Name() const {
  return "merge_context";
}

std::string MergeContextTool::Description() const {
  return "Merge the current context back to its parent. "
         "This creates a merge context that combines histories. "
         "Use when sub-task is complete and results should be integrated.";
}

std::string MergeContextTool::ParametersSchema() const {
  return R"schema({
    "type": "object",
    "properties": {
      "message": {
        "type": "string",
        "description": "Merge message describing what was accomplished"
      },
      "strategy": {
        "type": "string",
        "enum": ["merge", "squash"],
        "default": "merge",
        "description": "merge = keep all history; squash = keep only summary"
      }
    },
    "required": ["message"]
  })schema";
}

std::string MergeContextTool::Execute(const nlohmann::json& args, pu::ToolContext& ctx) {
  std::string message = args.value("message", "");
  std::string strategy = args.value("strategy", "merge");

  if (message.empty()) {
    return "Error: 'message' is required";
  }

  if (!ctx.fork_service) {
    return "Error: ForkMergeService not available in this context. "
           "Please use /merge command instead.";
  }
  if (!ctx.call_stack) {
    return "Error: CallStack not available in this context.";
  }

  if (ctx.request_confirmation) {
    std::string confirm_msg = "Merge current branch with strategy '" + strategy + "'?";
    if (!ctx.request_confirmation(confirm_msg)) {
      return "Merge cancelled by user.";
    }
  }

  if (ctx.call_stack->IsEmpty()) {
    return "Error: no active context to merge.";
  }

  auto child = ctx.call_stack->CurrentContext();
  auto result = ctx.fork_service->Merge(child, message, strategy);
  if (result.report.status != HandoffReceipt::Status::kFailed) {
    ctx.call_stack->Pop();
  }
  return result.message;
}

std::string ListForksTool::Name() const {
  return "list_forks";
}

std::string ListForksTool::Description() const {
  return "List all active and merged child contexts of the current context.";
}

std::string ListForksTool::ParametersSchema() const {
  return R"({"type": "object", "properties": {}})";
}

std::string ListForksTool::Execute(const nlohmann::json& args, pu::ToolContext& ctx) {
  (void)args;

  if (!ctx.fork_service) {
    return "Error: ForkMergeService not available in this context. "
           "Please use '/fork list' command to see all contexts.";
  }

  std::ostringstream oss;
  ctx.fork_service->PrintTree(oss);
  return oss.str();
}

}  // namespace pu::tools