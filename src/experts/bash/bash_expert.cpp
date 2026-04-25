// SPDX-License-Identifier: GPL-3.0-only

#include "bash_expert.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>

namespace pu::experts {

BashExpert::BashExpert(pu::backend::Backend* backend,
                       std::unique_ptr<pu::executor::CommandExecutor> executor)
    : backend_(backend), executor_(std::move(executor)) {}

void BashExpert::ResetSession() {
}

std::string BashExpert::Handle(const std::string& input,
                               pu::expert::ExpertContext& ctx) {
  (void)ctx;
  return RunToolLoop(input);
}

std::string BashExpert::RunToolLoop(const std::string& user_input) {
  if (!backend_->SupportsTools()) {
    return "This backend does not support tool calling. Cannot execute commands.";
  }

  using json = nlohmann::json;

  std::vector<pu::backend::Message> history;
  history.push_back({pu::backend::Message::Role::kUser, user_input});

  pu::backend::ToolDefinition bash_tool;
  bash_tool.name = "execute_bash";
  bash_tool.description = "Execute a safe Linux bash command and return the output.";
  bash_tool.parameters.raw_schema = R"({"type":"object","properties":{"command":{"type":"string","description":"The bash command to execute"}},"required":["command"]})";

  std::vector<pu::backend::ToolDefinition> tools = {bash_tool};

  std::string final_response;
  bool tool_was_called = false;

  do {
    tool_was_called = false;
    std::vector<pu::backend::ToolCall> collected_calls;
    std::ostringstream content_stream;

    backend_->Chat(history, tools,
      [&](pu::backend::TokenType type, std::string_view token, bool is_final) {
        if (type == pu::backend::TokenType::kContent) {
          if (!is_final) {
            std::cout << token << std::flush;
            content_stream << token;
          } else {
            std::cout << std::endl;
          }
        }
      },
      [&](const pu::backend::ToolCall& call) {
        tool_was_called = true;
        collected_calls.push_back(call);
      }
    );

    if (!tool_was_called) {
      final_response = content_stream.str();
      break;
    }

    // Ask for user confirmation before appending tool call to history
    std::cout << "[CONFIRM] Execute tool calls? [y/N] ";
    std::string confirm;
    std::getline(std::cin, confirm);
    if (confirm != "y" && confirm != "Y") {
      final_response = "Command execution cancelled by user.";
      break;
    }

    pu::backend::Message assistant_msg;
    assistant_msg.role = pu::backend::Message::Role::kAssistant;
    assistant_msg.tool_calls = collected_calls;
    history.push_back(assistant_msg);

    for (const auto& call : collected_calls) {
      std::string result;
      if (call.name == "execute_bash") {
        try {
          json args = json::parse(call.arguments);
          std::string command = args.value("command", "");
          if (command.empty()) {
            result = "No command provided.";
          } else {
            std::string reason;
            if (executor_->IsDangerous(command, &reason)) {
              result = "Blocked: " + reason;
            } else {
              auto exec_result = executor_->Execute(command);
              if (exec_result.was_intercepted) {
                result = "Blocked: " + exec_result.intercept_reason;
              } else if (exec_result.exit_code == 0) {
                result = "Success.\n" + exec_result.stdout_content;
              } else {
                result = "Failed (exit " + std::to_string(exec_result.exit_code) + ").\n"
                         + exec_result.stderr_content + exec_result.stdout_content;
              }
            }
          }
        } catch (const std::exception& e) {
          result = std::string("Argument parse error: ") + e.what();
        }
      } else {
        result = "Unknown tool: " + call.name;
      }

      pu::backend::Message tool_msg;
      tool_msg.role = pu::backend::Message::Role::kTool;
      tool_msg.tool_name = call.name;
      tool_msg.content = result;
      history.push_back(tool_msg);
    }
  } while (tool_was_called);

  return final_response;
}

}  // namespace pu::experts
