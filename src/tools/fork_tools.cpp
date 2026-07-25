// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/fork_tools.hpp"
#include "pu/core/context.hpp"

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

std::string ForkContextTool::Execute(const nlohmann::json& args, agent::ToolContext& ctx) {
  (void)ctx;

  std::string agent_name = args.value("agent_name", "");
  std::string goal = args.value("goal", "");
  std::string branch_name = args.value("branch_name", "");

  if (agent_name.empty()) {
    return "Error: 'agent_name' is required";
  }
  if (goal.empty()) {
    return "Error: 'goal' is required";
  }


  nlohmann::json result;
  result["action"] = "fork";
  result["agent_name"] = agent_name;
  result["goal"] = goal;
  if (!branch_name.empty()) {
    result["branch_name"] = branch_name;
  }

  return "Fork requested: agent='" + agent_name + "', goal='" + goal + "'\n"
         "Use /push command to execute the sub-task in a forked context.\n"
         "Result: " + result.dump();
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

std::string MergeContextTool::Execute(const nlohmann::json& args, agent::ToolContext& ctx) {
  (void)ctx;

  std::string message = args.value("message", "");
  std::string strategy = args.value("strategy", "merge");

  if (message.empty()) {
    return "Error: 'message' is required";
  }

  nlohmann::json result;
  result["action"] = "merge";
  result["message"] = message;
  result["strategy"] = strategy;

  return "Merge requested: message='" + message + "', strategy='" + strategy + "'\n"
         "Use /pop command to complete the sub-task and merge results.\n"
         "Result: " + result.dump();
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

std::string ListForksTool::Execute(const nlohmann::json& args, agent::ToolContext& ctx) {
  (void)args;
  (void)ctx;




  return "List forks requested. Use '/fork list' command to see all contexts.";
}

}  // namespace pu::tools
