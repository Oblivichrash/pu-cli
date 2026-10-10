// SPDX-License-Identifier: GPL-3.0-only
#include "pu/command_router.hpp"
#include "pu/session/session.hpp"
#include "pu/core/base.hpp"
#include "pu/core/text.hpp"
#include "pu/runtime.hpp"

#include <sstream>

namespace pu {

namespace {

constexpr const char* kBackendHelp =
    "  /backend <agent_name>  Switch to a predefined agent\n"
    "  /backend <type> <model> [host] [api_key]  Adopt a backend as an agent of its type\n";

}  // namespace

bool CommandRouter::RequireMinArgs(Args args, size_t min, const std::string& usage,
                                   Output output) const {
  if (args.size() < min) {
    output = usage;
    return true;
  }
  return false;
}

std::string CommandRouter::FormatUsage(const std::string& cmd, const std::string& usage) const {
  return "Usage: " + cmd + " " + usage;
}

const std::vector<std::string>& CommandRouter::CommandOrder() {
  static const std::vector<std::string> order = {"/help",     "/backend", "/agents",
                                                 "/clear",    "/rewind",  "/thinking"};
  return order;
}

const std::unordered_map<std::string, CommandRouter::Command>& CommandRouter::Commands() {
  static const std::unordered_map<std::string, Command> commands = {
      {"/help",
       {"  /help                  Show this help message\n",
        [this](Args, Session&, Output output) {
          output = GetHelpText();
          return true;
        }}},
      {"/backend",
       {kBackendHelp,
        [this](Args args, Session&, Output output) { return HandleBackend(args, output); }}},
      {"/agents",
       {"  /agents                List available agents\n",
        [this](Args args, Session& session, Output output) {
          return HandleAgents(args, session, output);
        }}},
      {"/clear",
       {"  /clear                 Clear conversation history\n",
        [](Args, Session& session, Output output) {
          session.GetConversation().ClearHistory();
          output = "Conversation history cleared.";
          return true;
        }}},
      {"/rewind",
       {"  /rewind <turn>         Step back to before a turn; the next message replaces it\n",
        [this](Args args, Session& session, Output output) {
          return HandleRewind(args, session, output);
        }}},
      {"/thinking",
       {"  /thinking              Show the thinking level this session asks for\n"
        "  /thinking <level>      none, low, medium, high, or default (the backend decides)\n"
        "  /thinking auto         Follow the agent's configuration again\n",
        [this](Args args, Session&, Output output) { return HandleThinking(args, output); }}},
  };
  return commands;
}

CommandRouter::CommandRouter(Runtime& runtime) : runtime_(runtime) {}

bool CommandRouter::Route(const std::string& input, Session& session, std::string& output) {
  const std::string trimmed(text::Trim(input));

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

  if (const auto it = Commands().find(cmd); it != Commands().end()) {
    return it->second.run(args, session, output);
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
  for (const auto& cmd : CommandOrder()) {
    oss << Commands().at(cmd).help;
  }
  oss << "  /exit, /quit           Exit the chat\n";
  return oss.str();
}

bool CommandRouter::HandleBackend(Args args, Output output) {
  if (args.empty()) {
    const config::BackendConfig cfg = runtime_.CurrentBackend();
    output = "Current backend: " + std::string(config::BackendTypeName(cfg.type)) +
             " (model: " + cfg.model + ", host: " + cfg.host + ")";
    return true;
  }

  const config::AgentEntry* agent_config = runtime_.GetAgentManager().GetAgentConfig(args[0]);
  if (agent_config) {
    try {
      runtime_.SwitchAgent(*agent_config);
      output = "Switched to agent: " + args[0] + " (" +
               std::string(config::BackendTypeName(agent_config->backend.type)) + "/" +
               agent_config->backend.model + ")";
    } catch (const std::exception& e) {
      output = "Error: " + std::string(e.what());
    }
    return true;
  }

  if (RequireMinArgs(args, 2, FormatUsage("/backend", "<type> <model> [host] [api_key]"), output))
    return true;

  const auto type = config::ParseBackendType(args[0]);
  if (!type) {
    output = "Unknown type: " + args[0] + ". Use " + config::BackendTypeNames() + ".";
    return true;
  }

  config::BackendConfig new_cfg;
  new_cfg.type = *type;
  new_cfg.model = args[1];
  if (args.size() > 2) new_cfg.host = args[2];
  if (args.size() > 3) new_cfg.api_key = args[3];
  if (new_cfg.host.empty()) new_cfg.host = config::DefaultHostFor(*type);

  try {
    runtime_.SwitchBackend(new_cfg);
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

bool CommandRouter::HandleAgents(Args, Session& session, Output output) {
  const std::string current = session.GetSpec().agent_name;
  std::ostringstream oss;
  oss << "Available agents:\n";
  for (const auto& name : runtime_.GetAgentManager().GetAgentNames()) {
    oss << "  " << name;
    if (name == current) oss << " (active)";
    const auto* cfg = runtime_.GetAgentManager().GetAgentConfig(name);
    if (cfg && !cfg->description.empty()) {
      oss << " - " << cfg->description;
    }
    oss << "\n";
  }
  output = oss.str();
  return true;
}

bool CommandRouter::HandleRewind(Args args, Session& session, Output output) {
  if (RequireMinArgs(args, 1, FormatUsage("/rewind", "<turn>"), output)) return true;

  size_t turn = 0;
  try {
    turn = static_cast<size_t>(std::stoul(args[0]));
  } catch (const std::exception&) {
    output = "Usage: /rewind <turn> (a positive number)";
    return true;
  }

  try {
    if (!session.GetConversation().RewindBefore(turn)) {
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

bool CommandRouter::HandleThinking(Args args, Output output) {
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
    if (!runtime_.SetThinkingLevel(std::nullopt)) {
      output = "This backend does not carry a thinking level.";
      return true;
    }
    output = "Thinking follows the agent's configuration again (" +
             std::string(ThinkingLevelName(runtime_.CurrentThinkingLevel())) + ").";
    return true;
  }

  if (args[0] != "default" && ParseThinkingLevel(args[0]) == ThinkingLevel::kServerDefault) {
    output =
        "Unknown thinking level: " + args[0] + ". Use none, low, medium, high, default or auto.";
    return true;
  }

  if (!runtime_.SetThinkingLevel(ParseThinkingLevel(args[0]))) {
    output = "This backend does not carry a thinking level.";
    return true;
  }
  output = "Thinking: " + std::string(ThinkingLevelName(runtime_.CurrentThinkingLevel())) +
           " for this session.";
  return true;
}

}  // namespace pu
