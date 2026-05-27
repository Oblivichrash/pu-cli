// SPDX-License-Identifier: GPL-3.0-only
#include "bash_agent.hpp"
#include "pu/renderer.hpp"
#include "pu/agent.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace pu::agents {

const std::string BashAgent::kDefaultSystemPrompt =
    "You have a tool 'write_file' that can write text content to a file. "
    "Prefer using 'write_file' over bash commands like 'cat >', 'echo >', or 'tee' for writing files. "
    "Only use 'execute_bash' for commands that cannot be done by 'write_file'.";

BashAgent::BashAgent(const std::string& name,
                     std::unique_ptr<pu::backend::Backend> backend,
                     std::unique_ptr<pu::executor::CommandExecutor> executor,
                     config::ConfirmationPolicy policy)
    : name_(name), backend_(std::move(backend)), executor_(std::move(executor)),
      sandbox_root_("."),  // default, can be overridden
      confirmation_policy_(policy) {}

void BashAgent::ResetSession() {
  history_.clear();
  recent_scores_.clear();
  user_approved_all_safe_ = false;
}

std::vector<pu::backend::Message> BashAgent::BuildInitialHistory() const {
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

void BashAgent::AppendTurnToHistory(
    const std::vector<pu::backend::Message>& history, size_t initial_size,
    std::vector<pu::ChatMessage>& turn_history) const {
  for (size_t i = initial_size + 1; i < history.size(); ++i) {
    const auto& msg = history[i];
    pu::ChatMessage cm;
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

std::string BashAgent::Handle(const std::string& input,
                              pu::agent::AgentContext& ctx) {
  if (ctx.system_prompt && !ctx.system_prompt->empty()) {
    bool has_system = false;
    for (const auto& cm : history_) {
      if (cm.role == "system") {
        has_system = true;
        break;
      }
    }
    if (!has_system) {
      history_.insert(history_.begin(),
                      {0, "", "system", *ctx.system_prompt, ""});
    }
  }

  auto initial = BuildInitialHistory();
  std::vector<pu::ChatMessage> turn_history;
  auto response = RunToolLoop(input, ctx.show_reasoning, turn_history, ctx, initial);
  history_.insert(history_.end(), turn_history.begin(), turn_history.end());
  return response;
}

std::string BashAgent::RunToolLoop(const std::string& user_input,
                                   bool show_reasoning,
                                   std::vector<pu::ChatMessage>& turn_history,
                                   pu::agent::AgentContext& ctx,
                                   const std::vector<pu::backend::Message>& initial_history) {
  if (!backend_->SupportsTools()) {
    return "This backend does not support tool calling. Cannot execute commands.";
  }

  using json = nlohmann::json;
  auto history = initial_history;
  history.push_back({pu::backend::Message::Role::kUser, user_input});

  pu::backend::ToolDefinition bash_tool;
  bash_tool.name = "execute_bash";
  bash_tool.description = "Execute a safe Linux bash command and return the output.";
  bash_tool.parameters.raw_schema = R"({"type":"object","properties":{"command":{"type":"string"}},"required":["command"]})";

  pu::backend::ToolDefinition write_tool;
  write_tool.name = "write_file";
  write_tool.description = "Write text content to a file (overwrites if exists). Use this instead of bash redirections.";
  write_tool.parameters.raw_schema = R"({
    "type": "object",
    "properties": {
      "path": {"type": "string", "description": "File path (relative to current directory)"},
      "content": {"type": "string", "description": "Text content to write"}
    },
    "required": ["path", "content"]
  })";

  std::vector<pu::backend::ToolDefinition> tools = {bash_tool, write_tool};

  std::string final_response;
  bool tool_was_called = false;

  do {
    tool_was_called = false;
    std::vector<pu::backend::ToolCall> collected_calls;
    std::ostringstream content_stream;
    auto renderer = pu::StreamingRenderer::Create(show_reasoning);
    std::error_code ec;

    backend_->Chat(history, tools,
      [&](pu::backend::TokenType type, std::string_view token, bool is_final) {
        if (type == pu::backend::TokenType::kContent) {
          renderer(type, token, is_final);
          if (!is_final) content_stream << token;
        } else if (type == pu::backend::TokenType::kReasoning) {
          renderer(type, token, is_final);
        }
      },
      [&](const pu::backend::ToolCall& call) {
        tool_was_called = true;
        collected_calls.push_back(call);
      }, ec);

    if (ec) {
      auto err = "Request failed: " + ec.message();
      std::cerr << "\nError: " << err << "\n";
      final_response = err;
      turn_history.push_back({0, "", "bash", final_response, ""});
      break;
    }
    if (!tool_was_called) {
      final_response = content_stream.str();
      turn_history.push_back({0, "", "bash", final_response, ""});
      break;
    }

    std::vector<std::string> commands;
    for (const auto& call : collected_calls) {
      if (call.name == "execute_bash") {
        try {
          auto args = json::parse(call.arguments);
          auto cmd = args.value("command", "");
          if (!cmd.empty()) commands.push_back(cmd);
        } catch (...) {}
      }
    }

    auto highest = pu::executor::RiskLevel::kSafe;
    for (const auto& cmd : commands) {
      auto risk = executor_->AssessRisk(cmd);
      if (risk.level > highest) highest = risk.level;
    }

    bool should_ask = true;
    if (confirmation_policy_ == config::ConfirmationPolicy::kNever ||
        (confirmation_policy_ == config::ConfirmationPolicy::kAutoSafe && highest == pu::executor::RiskLevel::kSafe) ||
        (user_approved_all_safe_ && highest == pu::executor::RiskLevel::kSafe)) {
      should_ask = false;
    }

    if (should_ask && !commands.empty()) {
      pu::agent::ConfirmationRequest req;
      req.highest_risk = highest;
      std::ostringstream desc;
      if (commands.size() == 1) desc << "Execute: " << commands[0];
      else {
        desc << "Execute these commands?\n";
        for (size_t i = 0; i < commands.size(); ++i)
          desc << "  " << (i + 1) << ". " << commands[i] << "\n";
      }
      req.description = desc.str();

      auto choice = ctx.request_confirmation(req);
      if (choice == pu::agent::ConfirmationChoice::kDenyAll) {
        final_response = "All command execution denied by user.";
        turn_history.push_back({0, "", "bash", final_response, ""});
        break;
      }
      if (choice == pu::agent::ConfirmationChoice::kDeny) {
        final_response = "Command execution cancelled by user.";
        turn_history.push_back({0, "", "bash", final_response, ""});
        break;
      }
      if (choice == pu::agent::ConfirmationChoice::kApproveAllSafe) {
        user_approved_all_safe_ = true;
      }
    }

    pu::backend::Message assistant_msg;
    assistant_msg.role = pu::backend::Message::Role::kAssistant;
    assistant_msg.tool_calls = collected_calls;
    history.push_back(assistant_msg);

    for (const auto& call : collected_calls) {
      std::string result;
      if (call.name == "execute_bash") {
        try {
          auto args = json::parse(call.arguments);
          auto command = args.value("command", "");
          if (command.empty()) {
            result = "No command provided.";
          } else {
            auto risk = executor_->AssessRisk(command);
            if (risk.level == pu::executor::RiskLevel::kDangerous) {
              result = "Blocked: " + risk.reason;
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
      } else if (call.name == "write_file") {
        try {
          auto args = json::parse(call.arguments);
          std::string path = args.value("path", "");
          std::string content = args.value("content", "");
          if (path.empty()) {
            result = "Error: 'path' is required";
          } else {
            std::filesystem::path full_path = std::filesystem::current_path() / path;
            // Simple security: prevent directory traversal out of current directory
            if (full_path.lexically_normal().string().find("..") != std::string::npos &&
                full_path.lexically_normal().string().find(full_path.lexically_normal().string()) != 0) {
              result = "Error: path traversal not allowed";
            } else {
              std::ofstream file(full_path);
              if (!file.is_open()) {
                result = "Error: cannot write to " + path;
              } else {
                file << content;
                result = "Successfully wrote " + std::to_string(content.size()) + " bytes to " + path;
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
      tool_msg.tool_name = call.id;
      tool_msg.content = result;
      history.push_back(tool_msg);
    }
  } while (tool_was_called);

  AppendTurnToHistory(history, initial_history.size(), turn_history);
  return final_response;
}

std::vector<pu::ChatMessage> BashAgent::SaveState() const { return history_; }
void BashAgent::LoadState(const std::vector<pu::ChatMessage>& messages) { history_ = messages; }

double BashAgent::EvaluateRelevance(const pu::ChatMessage& msg) {
  std::string lower = msg.content;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  double score = 0.0;
  if (lower.find("error") != std::string::npos) score += 0.4;
  if (lower.find("fail") != std::string::npos) score += 0.4;
  if (lower.find("urgent") != std::string::npos) score += 0.5;
  if (lower.find("crash") != std::string::npos) score += 0.5;
  if (lower.find("timeout") != std::string::npos) score += 0.3;
  return std::min(score, 1.0);
}

void BashAgent::OnPanelMessage(const pu::ChatMessage& msg) {
  double s = EvaluateRelevance(msg);
  if (s > 0.0) recent_scores_.push_back(s);
}

std::optional<std::string> BashAgent::ProactiveReply() {
  if (std::any_of(recent_scores_.begin(), recent_scores_.end(),
                  [this](double s) { return s >= proactive_threshold_; })) {
    recent_scores_.clear();
    return "I noticed a possible error. Reply @bash check logs to investigate.";
  }
  return std::nullopt;
}

void BashAgent::SetProactiveThreshold(double threshold) { proactive_threshold_ = threshold; }

}  // namespace pu::agents
