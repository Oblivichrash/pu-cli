// SPDX-License-Identifier: GPL-3.0-only

#include "bash_expert.hpp"
#include "pu/renderer.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <vector>

namespace pu::experts {

BashExpert::BashExpert(const std::string& name,
                       std::unique_ptr<pu::backend::Backend> backend,
                       std::unique_ptr<pu::executor::CommandExecutor> executor)
    : name_(name), backend_(std::move(backend)), executor_(std::move(executor)) {}

void BashExpert::ResetSession() {
  history_.clear();
}

std::vector<pu::backend::Message> BashExpert::BuildInitialHistory() const {
  std::vector<pu::backend::Message> initial;
  for (const auto& cm : history_) {
    pu::backend::Message msg;
    if (cm.role == "user") {
      msg.role = pu::backend::Message::Role::kUser;
    } else if (cm.role == "bash" || cm.role == "assistant") {
      msg.role = pu::backend::Message::Role::kAssistant;
    } else if (cm.role == "tool_result") {
      msg.role = pu::backend::Message::Role::kTool;
    } else {
      msg.role = pu::backend::Message::Role::kSystem;
    }
    msg.content = cm.content;
    initial.push_back(msg);
  }
  return initial;
}

void BashExpert::AppendTurnToHistory(const std::vector<pu::backend::Message>& history,
                                     size_t initial_size,
                                     std::vector<ChatMessage>& turn_history) const {
  for (size_t i = initial_size + 1; i < history.size(); ++i) {
    const auto& msg = history[i];
    ChatMessage cm;
    cm.id = 0;
    cm.timestamp = "";
    if (msg.role == pu::backend::Message::Role::kAssistant) {
      cm.role = "bash";
      if (!msg.content.empty()) {
        cm.content = msg.content;
      } else {
        std::ostringstream tool_summary;
        for (const auto& tc : msg.tool_calls) {
          tool_summary << "[ToolCall: " << tc.name << "(" << tc.arguments << ")]";
        }
        cm.content = tool_summary.str();
      }
    } else if (msg.role == pu::backend::Message::Role::kTool) {
      cm.role = "tool_result";
      cm.content = msg.content;
    } else {
      cm.role = "unknown";
      cm.content = msg.content;
    }
    turn_history.push_back(cm);
  }
}

std::string BashExpert::Handle(const std::string& input,
                               pu::expert::ExpertContext& ctx) {
  auto initial_history = BuildInitialHistory();

  std::vector<ChatMessage> turn_history;
  std::string response = RunToolLoop(input, ctx.show_reasoning, turn_history, initial_history);

  history_.insert(history_.end(), turn_history.begin(), turn_history.end());
  return response;
}

std::string BashExpert::RunToolLoop(const std::string& user_input,
                                    bool show_reasoning,
                                    std::vector<ChatMessage>& turn_history,
                                    const std::vector<pu::backend::Message>& initial_history) {
  if (!backend_->SupportsTools()) {
    return "This backend does not support tool calling. Cannot execute commands.";
  }

  using json = nlohmann::json;

  std::vector<pu::backend::Message> history = initial_history;
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
    auto renderer_cb = pu::StreamingRenderer::Create(show_reasoning);

    backend_->Chat(history, tools,
      [&](pu::backend::TokenType type, std::string_view token, bool is_final) {
        if (type == pu::backend::TokenType::kContent) {
          renderer_cb(type, token, is_final);
          if (!is_final) {
            content_stream << token;
          }
        } else if (type == pu::backend::TokenType::kReasoning) {
          renderer_cb(type, token, is_final);
        }
      },
      [&](const pu::backend::ToolCall& call) {
        tool_was_called = true;
        collected_calls.push_back(call);
      }
    );

    if (!tool_was_called) {
      final_response = content_stream.str();
      turn_history.push_back({0, "", "bash", final_response, ""});
      break;
    }

    std::vector<std::string> commands;
    for (const auto& call : collected_calls) {
      if (call.name == "execute_bash") {
        try {
          json args = json::parse(call.arguments);
          std::string cmd = args.value("command", "");
          if (!cmd.empty()) {
            commands.push_back(cmd);
          }
        } catch (...) {}
      }
    }

    if (commands.empty()) {
      std::cout << "[CONFIRM] Execute tool calls? [y/N] ";
    } else if (commands.size() == 1) {
      std::cout << "[CONFIRM] Execute: " << commands[0] << "? [y/N] ";
    } else {
      std::cout << "[CONFIRM] Execute these commands?\n";
      for (size_t i = 0; i < commands.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << commands[i] << "\n";
      }
      std::cout << "[y/N] ";
    }

    std::string confirm;
    std::getline(std::cin, confirm);
    if (confirm != "y" && confirm != "Y") {
      final_response = "Command execution cancelled by user.";
      turn_history.push_back({0, "", "bash", final_response, ""});
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

  AppendTurnToHistory(history, initial_history.size(), turn_history);

  return final_response;
}

std::vector<ChatMessage> BashExpert::SaveState() const {
  return history_;
}

void BashExpert::LoadState(const std::vector<ChatMessage>& messages) {
  history_ = messages;
}

void BashExpert::OnPanelMessage(const ChatMessage& msg) {
  std::string content_lower = msg.content;
  std::transform(content_lower.begin(), content_lower.end(), content_lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (content_lower.find("error") != std::string::npos ||
      content_lower.find("fail") != std::string::npos ||
      content_lower.find("urgent") != std::string::npos) {
    error_detected_ = true;
  }
}

std::optional<std::string> BashExpert::ProactiveReply() {
  if (error_detected_) {
    error_detected_ = false;
    return "I noticed a possible error. Would you like me to check the system logs or investigate?";
  }
  return std::nullopt;
}

}  // namespace pu::experts
