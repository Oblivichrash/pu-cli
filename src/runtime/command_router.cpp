// SPDX-License-Identifier: GPL-3.0-only
#include "pu/runtime/command_router.hpp"
#include "pu/session/session.hpp"
#include "pu/core/base.hpp"
#include "pu/core/text.hpp"
#include "pu/config/agent_summary.hpp"
#include "pu/runtime/runtime.hpp"
#include "pu/config/backend.hpp"
#include "pu/llm/model_listing.hpp"

#include <sstream>

namespace pu {

namespace {

constexpr const char* kBackendHelp =
    "  /backend <agent_name>  Switch to a predefined agent\n"
    "  /backend <type> <model> [host]  Adopt a backend as an agent of its type\n";

// What a failed command says in the transcript. Written once so the prefix is one fact: a
// prefix that changed in three of four places would read as two different failures.
std::string DescribeError(const std::exception& e) {
  return std::string("Error: ") + e.what();
}

}  // namespace

CommandReply CommandRouter::Replied(std::string text) {
  return {.text = std::move(text), .routed = true, .ok = true};
}

CommandReply CommandRouter::Failed(std::string text) {
  return {.text = std::move(text), .routed = true, .ok = false};
}

CommandReply CommandRouter::NotACommand() { return {}; }

// A usage line is a failure like any other: the caller printed it instead of an answer. Returning
// it rather than writing it means a handler that forgets this check cannot report success having
// said nothing, which is the pair of return values the old signature allowed.
CommandReply CommandRouter::RequireMinArgs(Args args, size_t min, const std::string& usage) const {
  if (args.size() < min) return Failed(usage);
  return Replied("");
}

std::string CommandRouter::FormatUsage(const std::string& cmd, const std::string& usage) const {
  return "Usage: " + cmd + " " + usage;
}

const std::vector<std::string>& CommandRouter::CommandOrder() {
  static const std::vector<std::string> order = {"/help",     "/backend", "/agents",
                                                 "/models",   "/clear",   "/rewind",
                                                 "/thinking"};
  return order;
}

CommandRouter::CommandRouter(Runtime& runtime) : runtime_(runtime) {
  commands_ = {
      {"/help",
       {"  /help                  Show this help message\n",
        [this](Args, Session&) { return Replied(Help()); }}},
      {"/backend",
       {kBackendHelp, [this](Args args, Session&) { return HandleBackend(args); }}},
      {"/agents",
       {"  /agents                List available agents\n",
        [this](Args args, Session& session) { return HandleAgents(args, session); }}},
      {"/models",
       {"  /models                List the models the active agent's backend reports\n"
        "  /models <agent>        List the models a named agent's backend reports\n",
        [this](Args args, Session&) { return HandleModels(args); }}},
      {"/clear",
       {"  /clear                 Clear conversation history\n",
        [](Args, Session& session) {
          session.Messages().ClearHistory();
          return Replied("Conversation history cleared.");
        }}},
      {"/rewind",
       {"  /rewind <turn>         Step back to before a turn; the next message replaces it\n",
        [this](Args args, Session& session) { return HandleRewind(args, session); }}},
      {"/thinking",
       {"  /thinking              Show the thinking level this session asks for\n"
        "  /thinking <level>      none, low, medium, high, or default (the backend decides)\n"
        "  /thinking auto         Follow the agent's configuration again\n",
        [this](Args args, Session&) { return HandleThinking(args); }}},
  };
}

CommandReply CommandRouter::Route(const std::string& input, Session& session) {
  const std::string trimmed(text::Trim(input));

  if (trimmed.empty() || trimmed[0] != '/') return NotACommand();

  const size_t space = trimmed.find(' ');
  const std::string cmd = space == std::string::npos ? trimmed : trimmed.substr(0, space);
  const std::string args_str = space == std::string::npos ? "" : trimmed.substr(space + 1);

  std::vector<std::string> args;
  if (!args_str.empty()) {
    std::istringstream iss(args_str);
    std::string arg;
    while (iss >> arg) {
      args.push_back(arg);
    }
  }

  if (const auto it = commands_.find(cmd); it != commands_.end()) {
    return it->second.run(args, session);
  }

  // Nothing to show, but a command: the chat reads this as "leave", and a caller that wanted
  // an answer gets an empty one rather than being told the line was not a command.
  if (cmd == "/exit" || cmd == "/quit") return Replied("");

  // A slash line no handler answered. It is a command -- the router was asked and said the
  // name means nothing -- so the text the chat prints is the remedy, not a provider's turn.
  return Failed("Unknown command: " + cmd);
}

std::string CommandRouter::Help() {
  std::ostringstream oss;
  oss << "Available commands:\n";
  for (const auto& cmd : CommandOrder()) {
    oss << commands_.at(cmd).help;
  }
  oss << "  /exit, /quit           Exit the chat\n";
  return oss.str();
}

CommandReply CommandRouter::HandleBackend(Args args) {
  if (args.empty()) {
    const config::Backend cfg = runtime_.CurrentBackend();
    return Replied("Current backend: " + std::string(config::GatewayName(cfg.gateway)) +
                   " (model: " + cfg.model + ", host: " + cfg.host + ")");
  }

  const config::AgentEntry* agent_config = runtime_.Agents().AgentConfig(args[0]);
  if (agent_config) {
    try {
      runtime_.SwitchAgent(*agent_config);
    } catch (const std::exception& e) {
      return Failed(DescribeError(e));
    }
    return Replied("Switched to agent: " + args[0] + " (" +
                   std::string(config::GatewayName(agent_config->backend.gateway)) + "/" +
                   agent_config->backend.model + ")");
  }

  const CommandReply enough =
      RequireMinArgs(args, 2, FormatUsage("/backend", "<type> <model> [host]"));
  if (!enough.text.empty()) return enough;

  // A key typed at a slash prompt is written into the scrollback and into whatever records
  // that terminal, so the command does not take one. An endpoint that needs a key is named
  // in agents.json, where it can come from the environment and stay out of the transcript.
  if (args.size() > 3) {
    return Failed("Usage: /backend <type> <model> [host]. A key is set in agents.json, not here.");
  }

  const auto type = config::ParseGatewayId(args[0]);
  if (!type) {
    return Failed("Unknown type: " + args[0] + ". Use " + config::GatewayNames() + ".");
  }

  config::Backend new_cfg;
  new_cfg.gateway = *type;
  new_cfg.model = args[1];
  if (args.size() > 2) new_cfg.host = args[2];
  if (new_cfg.host.empty()) new_cfg.host = config::DefaultHostFor(*type);

  try {
    runtime_.SwitchBackend(new_cfg);
  } catch (const std::exception& e) {
    return Failed(DescribeError(e));
  }
  return Replied("Switched backend to: " + args[0] + " (model: " + new_cfg.model +
                 ", host: " + new_cfg.host + ")");
}

CommandReply CommandRouter::HandleModels(Args args) {
  if (args.empty()) return ReportModels(runtime_.CurrentBackend());

  const config::AgentEntry* agent_config = runtime_.Agents().AgentConfig(args[0]);
  if (!agent_config) return Failed("Unknown agent: " + args[0]);
  return ReportModels(agent_config->backend);
}

CommandReply CommandRouter::ReportModels(const config::Backend& cfg) {
  try {
    const std::vector<llm::ModelEntry> models = config::ListModels(cfg, runtime_.Http());
    return Replied(
        llm::FormatModelList(models, config::GatewayName(cfg.gateway), cfg.host, cfg.model));
  } catch (const std::exception& e) {
    return Failed(DescribeError(e));
  }
}

CommandReply CommandRouter::HandleAgents(Args, Session& session) {
  // The same table `pu agents` prints: the chat is not a second place to learn who is
  // configured, and two renderings of one list drift.
  return Replied(
      config::FormatAgentTable(config::SummarizeAgents(runtime_, session.Spec().agent_name)));
}

CommandReply CommandRouter::HandleRewind(Args args, Session& session) {
  const CommandReply enough = RequireMinArgs(args, 1, FormatUsage("/rewind", "<turn>"));
  if (!enough.text.empty()) return enough;

  size_t turn = 0;
  try {
    turn = static_cast<size_t>(std::stoul(args[0]));
  } catch (const std::exception&) {
    return Failed("Usage: /rewind <turn> (a positive number)");
  }

  bool rewound = false;
  try {
    rewound = session.Messages().RewindBefore(turn);
  } catch (const std::exception& e) {
    return Failed(DescribeError(e));
  }
  if (!rewound) return Failed("There is no turn " + args[0] + " in this conversation.");

  return Replied("Stepped back to before turn " + args[0] +
                 ". The next message replaces it; the turns after it stay in the file until "
                 "one does.");
}

CommandReply CommandRouter::HandleThinking(Args args) {
  const auto current = [&] {
    return std::string(ThinkingLevelName(runtime_.CurrentThinkingLevel()));
  };

  if (args.empty()) {
    return Replied("Thinking: " + current() + (runtime_.ThinkingOverride()
                                                   ? " (set for this session)"
                                                   : " (from the agent's configuration)"));
  }

  if (args.size() > 1) return Failed(FormatUsage("/thinking", "[level|auto]"));

  if (args[0] == "auto") {
    runtime_.SetThinkingLevel(std::nullopt);
    return Replied("Thinking follows the agent's configuration again (" + current() + ").");
  }

  if (args[0] != "default" && ParseThinkingLevel(args[0]) == ThinkingLevel::kServerDefault) {
    return Failed("Unknown thinking level: " + args[0] +
                  ". Use none, low, medium, high, default or auto.");
  }

  runtime_.SetThinkingLevel(ParseThinkingLevel(args[0]));
  return Replied("Thinking: " + current() + " for this session.");
}

}  // namespace pu
