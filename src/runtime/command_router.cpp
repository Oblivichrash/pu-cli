// SPDX-License-Identifier: GPL-3.0-only
#include "pu/runtime/command_router.hpp"
#include "pu/session/workspace.hpp"
#include "pu/session/call_stack.hpp"
#include "pu/core/fork_merge_service.hpp"
#include "pu/conversation_store.hpp"
#include "pu/path_utils.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>

namespace pu {

CommandRouter::CommandRouter(agent::AgentManager& manager)
    : manager_(manager) {}

bool CommandRouter::Route(const std::string& input, Session& session, std::string& output) {
  // Trim leading whitespace
  std::string trimmed = input;
  size_t start = trimmed.find_first_not_of(" \t");
  if (start != std::string::npos) trimmed = trimmed.substr(start);
  
  if (trimmed.empty() || trimmed[0] != '/') return false;

  // Split into command and args
  size_t space = trimmed.find(' ');
  std::string cmd = space == std::string::npos ? trimmed : trimmed.substr(0, space);
  std::string args_str = space == std::string::npos ? "" : trimmed.substr(space + 1);
  
  // Parse args
  std::vector<std::string> args;
  if (!args_str.empty()) {
    std::istringstream iss(args_str);
    std::string arg;
    while (iss >> arg) {
      args.push_back(arg);
    }
  }

  if (cmd == "/help") return HandleHelp(args, session, output);
  if (cmd == "/fork") return HandleFork(args, session, output);
  if (cmd == "/merge") return HandleMerge(args, session, output);
  if (cmd == "/backend") return HandleBackend(args, session, output);
  if (cmd == "/agents") return HandleAgents(args, session, output);
  if (cmd == "/save") return HandleSave(args, session, output);
  if (cmd == "/load") return HandleLoad(args, session, output);
  if (cmd == "/list") return HandleList(args, session, output);
  if (cmd == "/export") return HandleExport(args, session, output);
  if (cmd == "/note") return HandleNote(args, session, output);
  if (cmd == "/clear") return HandleClear(args, session, output);
  if (cmd == "/reload-tools") return HandleReloadTools(args, session, output);
  if (cmd == "/stack") return HandleStack(args, session, output);
  if (cmd == "/exit" || cmd == "/quit") {
    output = "";
    return true;
  }

  return false;
}

bool CommandRouter::HandleHelp(const std::vector<std::string>& args, Session& session, std::string& output) {
  std::ostringstream oss;
  oss << "Available commands:\n"
      << "  /help                  Show this help message\n"
      << "  /fork [<agent>]        Fork a new branch with optional agent\n"
      << "  /fork list             List all branches\n"
      << "  /fork show <id>        Show details of a branch\n"
      << "  /fork prune            Remove merged branches\n"
      << "  /merge [--full]        Merge the current branch back\n"
      << "  /backend <agent_name>  Switch to a predefined agent\n"
      << "  /backend <type> <model> [host] [api_key]  Manually set backend\n"
      << "  /agents                List available agents\n"
      << "  /save [name]           Save conversation\n"
      << "  /load <id>             Load conversation\n"
      << "  /list                  List saved conversations\n"
      << "  /export <id>           Export conversation as Markdown\n"
      << "  /note add <text>       Add a note\n"
      << "  /note show             Show notes\n"
      << "  /clear                 Clear conversation history\n"
      << "  /reload-tools          Reload external tools\n"
      << "  /stack                 Show delegation stack\n"
      << "  /exit, /quit           Exit the chat\n";
  output = oss.str();
  return true;
}

bool CommandRouter::HandleFork(const std::vector<std::string>& args, Session& session, std::string& output) {
  // Get or create ForkMergeService
  auto& call_stack = session.GetCallStack();
  auto fork_service = call_stack.GetForkMergeService();
  
  if (!fork_service) {
    // Create one if it doesn't exist
    auto root_ctx = call_stack.GetRootContext();
    if (!root_ctx) {
      root_ctx = std::make_shared<Workspace>("root");
      call_stack.SetRootContext(root_ctx);
    }
    fork_service = std::make_shared<ForkMergeService>(manager_, 
        std::shared_ptr<CallStack>(&call_stack, [](void*){}), 
        root_ctx);
    call_stack.SetForkMergeService(fork_service);
  }

  if (args.empty()) {
    // /fork - show tree
    std::ostringstream oss;
    fork_service->PrintTree(oss);
    output = oss.str();
    return true;
  }

  if (args[0] == "list") {
    std::ostringstream oss;
    fork_service->PrintTree(oss);
    output = oss.str();
    return true;
  }

  if (args[0] == "show") {
    std::string fork_id = args.size() > 1 ? args[1] : "";
    if (fork_id.empty()) {
      output = "Usage: /fork show <id>";
      return true;
    }
    auto found = fork_service->FindContext(fork_id);
    if (!found) {
      output = "Context not found: " + fork_id;
      return true;
    }
    std::ostringstream oss;
    auto st = found->GetState();
    oss << "=== Context: " << found->GetId() << " ===\n";
    oss << "  Branch: " << found->GetBranchName() << "\n";
    oss << "  State: " << (st == Workspace::State::kActive ? "active" :
                           st == Workspace::State::kMerged ? "merged" : "abandoned") << "\n";
    oss << "  History: " << found->HistorySize() << " messages\n";
    oss << "  Tokens: ~" << found->GetTokenCount() << "\n";
    oss << "  Artifacts: " << found->GetArtifacts().size() << "\n";
    if (found->IsMergeCommit()) {
      oss << "  Merge commit: yes\n";
      oss << "  Parents: " << found->GetMergeParents().size() << "\n";
    }
    auto parent = found->GetParent();
    if (parent) oss << "  Parent: " << parent->GetId() << "\n";
    oss << "  Children: " << found->GetChildren().size() << "\n";
    auto recent = found->Recent(10);
    if (!recent.empty()) {
      oss << "\n  Recent messages:\n";
      for (const auto& msg : recent) {
        std::string preview = msg.content.substr(0, 100);
        oss << "    [" << msg.role << "] " << preview;
        if (msg.content.size() > 100) oss << "...";
        oss << "\n";
      }
    }
    output = oss.str();
    return true;
  }

  if (args[0] == "prune") {
    bool confirmed = (args.size() > 1 && (args[1] == "--yes" || args[1] == "-y"));
    size_t count = fork_service->PruneMerged();
    std::ostringstream oss;
    if (confirmed) {
      oss << "Pruned " << count << " merged branch(es).\n";
    } else {
      oss << "Found " << count << " merged branch(es). "
          << "Use /fork prune --yes to remove them.\n";
    }
    output = oss.str();
    return true;
  }

  // /fork <agent_name> - fork with specific agent
  std::string agent_name = args[0];
  auto result = fork_service->Fork(agent_name, "Exploration", "");
  if (result.child_context) {
    Assignment asgn;
    asgn.goal = "exploration";
    asgn.agent_name = agent_name;
    call_stack.Push(asgn, result.child_context);
    std::ostringstream oss;
    oss << "\xf0\x9f\x91\x8d Forked to branch: " << result.child_context->GetBranchName()
        << " (agent: " << agent_name << ")\n"
        << "   Type /merge to close.";
    output = oss.str();
  } else {
    output = "Error: failed to fork.";
  }
  return true;
}

bool CommandRouter::HandleMerge(const std::vector<std::string>& args, Session& session, std::string& output) {
  auto& call_stack = session.GetCallStack();
  auto fork_service = call_stack.GetForkMergeService();
  
  if (!fork_service) {
    output = "Error: no fork service available.";
    return true;
  }

  if (call_stack.IsEmpty()) {
    output = "Error: no active branch to merge.";
    return true;
  }

  bool full = (!args.empty() && args[0] == "--full");
  
  if (full) {
    auto result = fork_service->Merge("Merged with full history", "merge");
    std::ostringstream oss;
    oss << "\xf0\x9f\x91\x8d Merged: " << result.report.summary;
    output = oss.str();
  } else {
    auto current = call_stack.CurrentContext();
    if (!current) {
      output = "Error: no active context to merge.";
      return true;
    }
    std::ostringstream oss;
    oss << "\xf0\x9f\x93\x8b Merge Strategy\n"
        << "   Branch: " << current->GetBranchName() << "\n"
        << "   History: " << current->HistorySize() << " messages, ~"
        << current->GetTokenCount() << " tokens\n";
    auto parent = current->GetParent();
    oss << "   Parent: " << (parent ? parent->GetBranchName() : "root") << "\n\n"
        << "   [s] Squash: Only summary (~50 tokens)\n"
        << "   [f] Full: Keep all history (~" << current->GetTokenCount() << " tokens)\n"
        << "   [c] Cancel: Discard this branch\n\n"
        << "   Choose strategy: ";
    output = oss.str();
    // Note: interactive strategy selection should be handled in cli layer
  }
  return true;
}

bool CommandRouter::HandleBackend(const std::vector<std::string>& args, Session& session, std::string& output) {
  // No arguments: display current backend
  if (args.empty()) {
    const auto& cfg = session.GetRuntimeSpec().backend;
    output = "Current backend: " + cfg.type +
             " (model: " + cfg.model +
             ", host: " + cfg.host + ")";
    return true;
  }

  // 1. Try to interpret as agent name
  const agent::config::AgentEntry* agent_config = manager_.GetAgentConfig(args[0]);
  if (agent_config) {
    BackendConfig new_cfg;
    new_cfg.type = (agent_config->backend.type == agent::config::BackendType::kOllama) ? "ollama" : "openai";
    new_cfg.host = agent_config->backend.host;
    new_cfg.model = agent_config->backend.model;
    new_cfg.api_key = agent_config->backend.api_key.value_or("");
    new_cfg.temperature = agent_config->backend.temperature;
    new_cfg.max_tokens = agent_config->backend.max_tokens;
    new_cfg.parameters_as_string = agent_config->backend.parameters_as_string;
    try {
      session.SwitchBackend(new_cfg);
      output = "Switched to agent: " + args[0] + " (" + new_cfg.type + "/" + new_cfg.model + ")";
    } catch (const std::exception& e) {
      output = "Error: " + std::string(e.what());
    }
    return true;
  }

  // 2. Manual mode: /backend <type> <model> [host] [api_key]
  if (args.size() < 2) {
    output = "Usage: /backend <agent_name> | /backend <type> <model> [host] [api_key]";
    return true;
  }

  BackendConfig new_cfg;
  new_cfg.type = args[0];
  new_cfg.model = args[1];
  if (args.size() > 2) {
    new_cfg.host = args[2];
  } else {
    // Provide default hosts
    if (new_cfg.type == "ollama") {
      new_cfg.host = "http://localhost:11434";
    } else if (new_cfg.type == "openai") {
      new_cfg.host = "https://api.openai.com/v1";
    } else {
      output = "Unknown type: " + new_cfg.type + ". Use 'ollama' or 'openai'.";
      return true;
    }
  }
  // Optional api_key
  if (args.size() > 3) {
    new_cfg.api_key = args[3];
  }

  try {
    session.SwitchBackend(new_cfg);
    output = "Switched backend to: " + new_cfg.type + " (model: " + new_cfg.model + ", host: " + new_cfg.host + ")";
    if (!new_cfg.api_key.empty()) {
      output += " (API key set)";
    }
  } catch (const std::exception& e) {
    output = "Error: " + std::string(e.what());
  }
  return true;
}

bool CommandRouter::HandleAgents(const std::vector<std::string>& args, Session& session, std::string& output) {
  auto names = manager_.GetAgentNames();
  std::string current = session.GetRuntimeSpec().agent_name;
  std::ostringstream oss;
  oss << "Available agents:\n";
  for (const auto& name : names) {
    oss << "  " << name;
    if (name == current) oss << " (active)";
    // Try to get description from config
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
  bool no_summary = false;
  
  for (const auto& arg : args) {
    if (arg == "--no-summary") {
      no_summary = true;
    } else {
      save_name = arg;
    }
  }
  
  if (save_name.empty()) {
    // Generate a simple ID
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ss;
    ss << in_time_t;
    save_name = ss.str();
  }

  // Save conversation to conversation store
  auto pu_dir = pu::path::GetDataDir();
  auto store_dir = pu_dir / "conversations";
  ConversationStore store(store_dir);
  
  Conversation conv;
  conv.id = save_name;
  conv.created_at = "";
  conv.updated_at = "";
  conv.messages = session.GetWorkspace().GetHistory();
  conv.expert_histories = {};
  
  try {
    store.Save(conv);
    output = "Conversation saved as '" + save_name + "'";
  } catch (const std::exception& e) {
    output = std::string("Error saving conversation: ") + e.what();
  }
  return true;
}

bool CommandRouter::HandleLoad(const std::vector<std::string>& args, Session& session, std::string& output) {
  if (args.empty()) {
    output = "Error: conversation id required.";
    return true;
  }

  auto pu_dir = pu::path::GetDataDir();
  auto store_dir = pu_dir / "conversations";
  ConversationStore store(store_dir);
  
  try {
    auto conv = store.Load(args[0]);
    auto& ws = session.GetWorkspace();
    // Clear existing and load
    for (const auto& msg : conv.messages) {
      ws.Append(msg);
    }
    output = "Loaded conversation '" + args[0] + "'";
  } catch (const std::exception& e) {
    output = std::string("Error loading conversation: ") + e.what();
  }
  return true;
}

bool CommandRouter::HandleList(const std::vector<std::string>& args, Session& session, std::string& output) {
  auto pu_dir = pu::path::GetDataDir();
  auto store_dir = pu_dir / "conversations";
  ConversationStore store(store_dir);
  
  std::vector<std::string> errors;
  auto convs = store.List(errors);
  
  std::ostringstream oss;
  if (convs.empty()) {
    oss << "No saved conversations.";
  } else {
    oss << "Saved conversations:\n";
    for (const auto& conv : convs) {
      oss << "  " << conv.id << " (" << conv.messages.size() << " messages)\n";
    }
  }
  if (!errors.empty()) {
    for (const auto& err : errors) {
      oss << "  [Warning] " << err << "\n";
    }
  }
  output = oss.str();
  return true;
}

bool CommandRouter::HandleExport(const std::vector<std::string>& args, Session& session, std::string& output) {
  if (args.empty()) {
    output = "Error: conversation id required.";
    return true;
  }

  auto pu_dir = pu::path::GetDataDir();
  auto store_dir = pu_dir / "conversations";
  ConversationStore store(store_dir);
  
  try {
    auto md = store.ExportMarkdown(args[0]);
    std::string filename = "conversation_" + args[0] + ".md";
    std::ofstream out(filename);
    if (!out) {
      output = "Error: cannot write to " + filename;
    } else {
      out << md;
      output = "Exported to " + filename;
    }
  } catch (const std::exception& e) {
    output = std::string("Error exporting conversation: ") + e.what();
  }
  return true;
}

bool CommandRouter::HandleNote(const std::vector<std::string>& args, Session& session, std::string& output) {
  if (args.empty()) {
    output = "Usage: /note add <text> | /note show";
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
    // Reconstruct the text from remaining args
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

  output = "Usage: /note add <text> | /note show";
  return true;
}

bool CommandRouter::HandleClear(const std::vector<std::string>& args, Session& session, std::string& output) {
  // Clear the session's workspace history instead of using agent sessions
  session.GetWorkspace().ClearHistory();
  output = "Conversation history cleared.";
  return true;
}

bool CommandRouter::HandleReloadTools(const std::vector<std::string>& args, Session& session, std::string& output) {
  output = "Tool reload not yet implemented.";
  return true;
}

bool CommandRouter::HandleStack(const std::vector<std::string>& args, Session& session, std::string& output) {
  auto& call_stack = session.GetCallStack();
  if (!call_stack.IsEmpty()) {
    std::ostringstream oss;
    oss << "Assignment stack (depth " << call_stack.Depth() << "):\n";
    for (const auto& frame : call_stack.GetFrames()) {
      oss << "  " << frame.assignment.agent_name
          << " [" << frame.assignment.id << "]\n";
      if (frame.context) {
        oss << "    Context: " << frame.context->GetId()
            << " [branch: " << frame.context->GetBranchName() << "]\n";
      }
    }
    output = oss.str();
  } else {
    output = "Assignment stack is empty (depth 0)";
  }
  return true;
}

} // namespace pu