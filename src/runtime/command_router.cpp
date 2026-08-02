// SPDX-License-Identifier: GPL-3.0-only
#include "pu/runtime/command_router.hpp"
#include "pu/session/workspace.hpp"
#include "pu/session/call_stack.hpp"
#include "pu/storage/session_store.hpp"
#include "pu/path_utils.hpp"
#include "pu/runtime/runtime.hpp"

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

SessionStore CommandRouter::GetSessionStore() const {
  return SessionStore(pu::path::GetDataDir() / "sessions");
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
  add("/save", &CommandRouter::HandleSave, "  /save [name]           Save conversation\n");
  add("/load", &CommandRouter::HandleLoad, "  /load <id>             Load conversation\n");
  add("/list", &CommandRouter::HandleList, "  /list                  List saved conversations\n");
  add("/export", &CommandRouter::HandleExport,
      "  /export <id>           Export conversation as Markdown\n");
  add("/note", &CommandRouter::HandleNote,
      "  /note add <text>       Add a note\n"
      "  /note show             Show notes\n");
  add("/clear", &CommandRouter::HandleClear, "  /clear                 Clear conversation history\n");
  add("/stack", &CommandRouter::HandleStack, "  /stack                 Show delegation stack\n");
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

bool CommandRouter::HandleHelp(const std::vector<std::string>& /*args*/, Session& /*session*/, std::string& output) {
  output = GetHelpText();
  return true;
}

bool CommandRouter::HandleBackend(const std::vector<std::string>& args, Session& session, std::string& output) {
  if (args.empty()) {
    const auto& cfg = session.GetRuntimeSpec().backend;
    output = "Current backend: " +
             std::string(cfg.type == config::BackendType::kOpenAI ? "openai" : "ollama") +
             " (model: " + cfg.model +
             ", host: " + cfg.host + ")";
    return true;
  }

  const config::AgentEntry* agent_config = manager_.GetAgentConfig(args[0]);
  if (agent_config) {
    // Directly use the agent's BackendConfig
    try {
      session.SwitchBackend(agent_config->backend);
      runtime_.SwitchAgent(*agent_config);
      output = "Switched to agent: " + args[0] + " (" +
        std::string(agent_config->backend.type == config::BackendType::kOpenAI ? "openai" : "ollama") +
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
    session.SwitchBackend(new_cfg);
    output = "Switched backend to: " + args[0] +
      " (model: " + new_cfg.model + ", host: " + new_cfg.host + ")";
    if (new_cfg.api_key && !new_cfg.api_key->empty()) {
      output += " (API key set)";
    }
  } catch (const std::exception& e) {
    output = "Error: " + std::string(e.what());
  }
  return true;
}

bool CommandRouter::HandleAgents(const std::vector<std::string>& /*args*/, Session& session, std::string& output) {
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

bool CommandRouter::HandleSave(const std::vector<std::string>& args, Session& session, std::string& output) {
  std::string save_name;

  for (const auto& arg : args) {
    if (arg == "--no-summary") continue;  // Accepted for CLI compatibility; summary is not generated on save.
    save_name = arg;
  }

  if (save_name.empty()) {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ss;
    ss << in_time_t;
    save_name = ss.str();
  }

  auto store = GetSessionStore();

  auto session_copy = Session(save_name, "cli");
  auto history = session.GetWorkspace().GetHistory();
  for (const auto& msg : history) {
    ChatMessage session_msg;
    session_msg.id = msg.id;
    session_msg.timestamp = msg.timestamp;
    session_msg.role = msg.role;
    session_msg.content = msg.content;
    session_msg.tool_name = msg.tool_name;
    session_msg.tool_calls_json = msg.tool_calls_json;
    session_copy.GetWorkspace().Append(session_msg);
  }

  try {
    store.SaveSession(session_copy);
    output = "Conversation saved as '" + save_name + "'";
  } catch (const std::exception& e) {
    output = std::string("Error saving conversation: ") + e.what();
  }
  return true;
}

bool CommandRouter::HandleLoad(const std::vector<std::string>& args, Session& session, std::string& output) {
  if (RequireMinArgs(args, 1, FormatUsage("/load", "<id>"), output))
    return true;

  auto store = GetSessionStore();

  try {
    auto loaded = store.LoadSession(args[0]);
    if (!loaded) {
      output = "Error: session not found: " + args[0];
      return true;
    }
    auto& ws = session.GetWorkspace();
    auto history = loaded->GetWorkspace().GetHistory();
    for (const auto& msg : history) {
      ws.Append(msg);
    }
    output = "Loaded conversation '" + args[0] + "'";
  } catch (const std::exception& e) {
    output = std::string("Error loading conversation: ") + e.what();
  }
  return true;
}

bool CommandRouter::HandleList(const std::vector<std::string>& /*args*/, Session& /*session*/, std::string& output) {
  auto store = GetSessionStore();
  auto metadata = store.ListAllMetadata();

  std::ostringstream oss;
  if (metadata.empty()) {
    oss << "No saved conversations.";
  } else {
    oss << "Saved conversations:\n";
    for (const auto& meta : metadata) {
      oss << "  " << meta.id << " (created: " << meta.created_at << ")\n";
    }
  }
  output = oss.str();
  return true;
}

bool CommandRouter::HandleExport(const std::vector<std::string>& args, Session& /*session*/, std::string& output) {
  if (RequireMinArgs(args, 1, FormatUsage("/export", "<id>"), output))
    return true;

  auto store = GetSessionStore();

  try {
    auto loaded = store.LoadSession(args[0]);
    if (!loaded) {
      output = "Error: session not found: " + args[0];
      return true;
    }

    std::string filename = "conversation_" + args[0] + ".md";
    std::ofstream out(filename);
    if (!out) {
      output = "Error: cannot write to " + filename;
    } else {
      out << "# Conversation: " << loaded->GetId() << "\n\n";
      auto history = loaded->GetWorkspace().GetHistory();
      for (const auto& msg : history) {
        out << "**" << msg.role << "** (" << msg.timestamp << "):\n\n" << msg.content << "\n\n---\n\n";
      }
      output = "Exported to " + filename;
    }
  } catch (const std::exception& e) {
    output = std::string("Error exporting conversation: ") + e.what();
  }
  return true;
}

bool CommandRouter::HandleNote(const std::vector<std::string>& args, Session& session, std::string& output) {
  if (args.empty()) {
    output = FormatUsage("/note", "add <text> | show");
    return true;
  }

  auto& ws = session.GetWorkspace();

  if (args[0] == "show") {
    std::string var_key = "notes/" + session.GetRuntimeSpec().agent_name;
    auto val = ws.GetVar(var_key);
    if (!val.has_value() || !val->is_array() || val->empty()) {
      output = "No notes yet.";
    } else {
      std::ostringstream oss;
      oss << "Notes for " << session.GetRuntimeSpec().agent_name << ":\n";
      for (const auto& item : *val) {
        if (item.is_string()) oss << item.get<std::string>() << "\n";
      }
      output = oss.str();
    }
    return true;
  }

  if (args[0] == "add" && args.size() > 1) {
    std::string text;
    for (size_t i = 1; i < args.size(); ++i) {
      if (i > 1) text += " ";
      text += args[i];
    }

    std::string var_key = "notes/" + session.GetRuntimeSpec().agent_name;
    auto existing = ws.GetVar(var_key);
    nlohmann::json new_notes = existing.has_value() ? *existing : nlohmann::json::array();
    if (new_notes.is_array()) {
      new_notes.push_back(text);
      ws.SetVar(var_key, new_notes);
    }
    output = "Note added.";
    return true;
  }

  output = FormatUsage("note", "add <text> | show");
  return true;
}

bool CommandRouter::HandleClear(const std::vector<std::string>& /*args*/, Session& session, std::string& output) {
  session.GetWorkspace().ClearHistory();
  output = "Conversation history cleared.";
  return true;
}

bool CommandRouter::HandleStack(const std::vector<std::string>& /*args*/, Session& session, std::string& output) {
  auto& call_stack = session.GetCallStack();
  if (!call_stack.IsEmpty()) {
    std::ostringstream oss;
    oss << "Assignment stack (depth " << call_stack.Depth() << "):\n";
    for (const auto& frame : call_stack.GetFrames()) {
      oss << "  " << frame.assignment.agent_name
          << " [" << frame.assignment.id << "]\n";
      if (frame.workspace) {
        oss << "    Workspace: " << frame.workspace->GetId()
            << " [branch: " << frame.workspace->GetBranchName() << "]\n";
      }
    }
    output = oss.str();
  } else {
    output = "Assignment stack is empty (depth 0)";
  }
  return true;
}

} // namespace pu
