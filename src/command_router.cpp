// SPDX-License-Identifier: GPL-3.0-only
#include "pu/command_router.hpp"
#include "pu/session/session.hpp"
#include "pu/core/base.hpp"
#include "pu/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>

namespace pu {

bool CommandRouter::RequireMinArgs(const std::vector<std::string>& args, size_t min,
                                   const std::string& usage, std::string& output) const {
  if (args.size() < min) {
    output = usage;
    return true;
  }
  return false;
}

std::string CommandRouter::FormatUsage(const std::string& cmd, const std::string& usage) const {
  return "Usage: " + cmd + " " + usage;
}

CommandRouter::Registry CommandRouter::BuildRegistry() {
  Registry reg;
  auto add = [&reg](const std::string& cmd, CommandHandler handler, const std::string& help) {
    reg.commands[cmd] = CommandEntry{handler, help};
    reg.order.push_back(cmd);
  };
  add("/help", &CommandRouter::HandleHelp, "  /help                  Show this help message\n");
  add("/backend", &CommandRouter::HandleBackend,
      "  /backend <agent_name>  Switch to a predefined agent\n"
      "  /backend <type> <model> [host] [api_key]  Manually set backend\n");
  add("/agents", &CommandRouter::HandleAgents, "  /agents                List available agents\n");
  add("/clear", &CommandRouter::HandleClear,
      "  /clear                 Clear conversation history\n");
  add("/rewind", &CommandRouter::HandleRewind,
      "  /rewind <turn>         Step back to before a turn; the next message replaces it\n");
  add("/thinking", &CommandRouter::HandleThinking,
      "  /thinking              Show the thinking level this session asks for\n"
      "  /thinking <level>      none, low, medium, high, or default (the backend decides)\n"
      "  /thinking auto         Follow the agent's configuration again\n");
  return reg;
}

const CommandRouter::Registry CommandRouter::kRegistry = CommandRouter::BuildRegistry();

CommandRouter::CommandRouter(AgentManager& manager, Runtime& runtime)
    : manager_(manager), runtime_(runtime) {}

bool CommandRouter::Route(const std::string& input, Session& session, std::string& output) {
  std::string trimmed = input;
  size_t start = trimmed.find_first_not_of(" \t");
  if (start != std::string::npos) trimmed = trimmed.substr(start);

  if (trimmed.empty() || trimmed[0] != '/') return false;

  size_t space = trimmed.find(' ');
  std::string cmd = space == std::string::npos ? trimmed : trimmed.substr(0, space);
  std::string args_str = space == std::string::npos ? "" : trimmed.substr(space + 1);

  std::vector<std::string> args;
  if (!args_str.empty()) {
    std::istringstream iss(args_str);
    std::string arg;
    while (iss >> arg) {
      args.push_back(arg);
    }
  }

  auto it = kRegistry.commands.find(cmd);
  if (it != kRegistry.commands.end()) {
    return (this->*(it->second.handler))(args, session, output);
  }

  if (cmd == "/exit" || cmd == "/quit") {
    output = "";
    return true;
  }

  return false;
}

std::string CommandRouter::GetHelpText() {
  std::ostringstream oss;
  oss << "Available commands:\n";
  for (const auto& cmd : kRegistry.order) {
    oss << kRegistry.commands.at(cmd).help;
  }
  oss << "  /exit, /quit           Exit the chat\n";
  return oss.str();
}

bool CommandRouter::HandleHelp(const std::vector<std::string>& /*args*/, Session& /*session*/,
                               std::string& output) {
  output = GetHelpText();
  return true;
}

bool CommandRouter::HandleBackend(const std::vector<std::string>& args, Session& session,
                                  std::string& output) {
  if (args.empty()) {
    const config::BackendConfig cfg = runtime_.CurrentBackend();
    output = "Current backend: " +
             std::string(cfg.type == config::BackendType::kOpenAI ? "openai" : "ollama") +
             " (model: " + cfg.model + ", host: " + cfg.host + ")";
    return true;
  }

  const config::AgentEntry* agent_config = manager_.GetAgentConfig(args[0]);
  if (agent_config) {
    try {
      runtime_.SwitchAgent(*agent_config);
      output = "Switched to agent: " + args[0] + " (" +
               std::string(agent_config->backend.type == config::BackendType::kOpenAI ? "openai"
                                                                                      : "ollama") +
               "/" + agent_config->backend.model + ")";
    } catch (const std::exception& e) {
      output = "Error: " + std::string(e.what());
    }
    return true;
  }

  if (RequireMinArgs(args, 2, FormatUsage("/backend", "<type> <model> [host] [api_key]"), output))
    return true;

  config::BackendConfig new_cfg;
  if (args[0] == "ollama") {
    new_cfg.type = config::BackendType::kOllama;
  } else if (args[0] == "openai") {
    new_cfg.type = config::BackendType::kOpenAI;
  } else {
    output = "Unknown type: " + args[0] + ". Use 'ollama' or 'openai'.";
    return true;
  }
  new_cfg.model = args[1];
  if (args.size() > 2) {
    new_cfg.host = args[2];
  } else {
    if (new_cfg.type == config::BackendType::kOllama) {
      new_cfg.host = "http://localhost:11434";
    } else {
      new_cfg.host = "https://api.openai.com/v1";
    }
  }
  if (args.size() > 3) {
    new_cfg.api_key = args[3];
  }

  try {
    session.SetBackendOverride(new_cfg);
    output = "Switched backend to: " + args[0] + " (model: " + new_cfg.model +
             ", host: " + new_cfg.host + ")";
    if (new_cfg.api_key && !new_cfg.api_key->empty()) {
      output += " (API key set)";
    }
  } catch (const std::exception& e) {
    output = "Error: " + std::string(e.what());
  }
  return true;
}

bool CommandRouter::HandleAgents(const std::vector<std::string>& /*args*/, Session& session,
                                 std::string& output) {
  auto names = manager_.GetAgentNames();
  std::string current = session.GetRuntimeSpec().agent_name;
  std::ostringstream oss;
  oss << "Available agents:\n";
  for (const auto& name : names) {
    oss << "  " << name;
    if (name == current) oss << " (active)";
    const auto* cfg = manager_.GetAgentConfig(name);
    if (cfg && !cfg->description.empty()) {
      oss << " - " << cfg->description;
    }
    oss << "\n";
  }
  output = oss.str();
  return true;
}

bool CommandRouter::HandleClear(const std::vector<std::string>& /*args*/, Session& session,
                                std::string& output) {
  session.GetWorkspace().ClearHistory();
  output = "Conversation history cleared.";
  return true;
}

bool CommandRouter::HandleRewind(const std::vector<std::string>& args, Session& session,
                                 std::string& output) {
  if (RequireMinArgs(args, 1, FormatUsage("/rewind", "<turn>"), output)) return true;

  size_t turn = 0;
  try {
    turn = static_cast<size_t>(std::stoul(args[0]));
  } catch (const std::exception&) {
    output = "Usage: /rewind <turn> (a positive number)";
    return true;
  }

  try {
    if (!session.GetWorkspace().RewindBefore(turn)) {
      output = "There is no turn " + args[0] + " in this conversation.";
      return true;
    }
    output = "Stepped back to before turn " + args[0] +
             ". The next message replaces it; the turns after it stay in the file until one does.";
  } catch (const std::exception& e) {
    output = "Error: " + std::string(e.what());
  }
  return true;
}

bool CommandRouter::HandleThinking(const std::vector<std::string>& args, Session& /*session*/,
                                   std::string& output) {
  if (!runtime_.SupportsThinkingLevel()) {
    output = "This backend does not carry a thinking level.";
    return true;
  }

  if (args.empty()) {
    output = "Thinking: " + std::string(ThinkingLevelName(runtime_.CurrentThinkingLevel()));
    output += runtime_.GetThinkingOverride() ? " (set for this session)"
                                             : " (from the agent's configuration)";
    return true;
  }

  if (args.size() > 1) {
    output = FormatUsage("/thinking", "[level|auto]");
    return true;
  }

  if (args[0] == "auto") {
    runtime_.SetThinkingLevel(std::nullopt);
    output = "Thinking follows the agent's configuration again (" +
             std::string(ThinkingLevelName(runtime_.CurrentThinkingLevel())) + ").";
    return true;
  }

  // An unrecognised word reads as the absent level, so the word itself is checked
  // rather than trusting that fallback to mean what was typed.
  if (args[0] != "default" && ParseThinkingLevel(args[0]) == ThinkingLevel::kServerDefault) {
    output =
        "Unknown thinking level: " + args[0] + ". Use none, low, medium, high, default or auto.";
    return true;
  }

  runtime_.SetThinkingLevel(ParseThinkingLevel(args[0]));
  output = "Thinking: " + std::string(ThinkingLevelName(runtime_.CurrentThinkingLevel())) +
           " for this session.";
  return true;
}

}  // namespace pu