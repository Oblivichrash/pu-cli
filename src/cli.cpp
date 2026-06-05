// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli.hpp"

#include "pu/agent.hpp"
#include "pu/agent_config.hpp"
#include "pu/agent_factory.hpp"
#include "pu/backend.hpp"
#include "pu/conversation_store.hpp"
#include "pu/context.hpp"
#include "pu/orchestrator.hpp"
#include "pu/stack.hpp"
#include "pu/renderer.hpp"

#include "agents/llm/llm_agent.hpp"
#include "http/curl_http_client.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace pu::cli {

namespace {

struct AppContext {
  config::AgentsConfig config;
  agent::AgentManager manager;
  std::string config_path;
};

AppContext SetupAppContext(const std::string& requested_expert, bool show_reasoning) {
  AppContext ctx;
  try {
    ctx.config_path = config::FindConfigPath();
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    std::exit(1);
  }

  std::error_code ec;
  ctx.config = config::LoadAgentsConfig(ctx.config_path, ec);
  if (ec) {
    std::cerr << "Error: failed to load config: " << ec.message() << "\n";
    std::exit(1);
  }

  if (ctx.config.experts.empty()) {
    std::cerr << "Error: no agents configured\n";
    std::exit(1);
  }

  auto active_name = requested_expert.empty() ? ctx.config.default_expert : requested_expert;
  bool active_found = false;

  for (const auto& entry : ctx.config.experts) {
    if (entry.name == active_name) active_found = true;
    try {
      ctx.manager.RegisterAgent(agent::AgentRegistry::Instance().CreateAgent(entry));
    } catch (const std::exception& e) {
      std::cerr << "Error: failed to create agent '" << entry.name << "': " << e.what() << "\n";
      std::exit(1);
    }
  }

  if (!active_found) {
    std::cerr << "Error: agent '" << active_name << "' not found\nAvailable agents:\n";
    for (const auto& e : ctx.config.experts) std::cerr << "  " << e.name << "\n";
    std::exit(1);
  }

  ctx.manager.SetActiveAgent(active_name);
  if (show_reasoning) ctx.manager.SetShowReasoning(true);
  return ctx;
}

inline std::string Trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t");
  if (start == std::string::npos) return {};
  auto end = s.find_last_not_of(" \t");
  return s.substr(start, end - start + 1);
}

std::string CurrentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ss;
  ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

std::string GenerateId() {
  auto now = std::chrono::high_resolution_clock::now();
  auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  std::ostringstream ss;
  ss << std::hex << nanos;
  return "conv-" + ss.str();
}

void PrintAgents(const pu::config::AgentsConfig& config, const std::string& current) {
  std::cout << "Available agents:\n";
  for (const auto& entry : config.experts) {
    std::cout << "  " << entry.name;
    if (!entry.description.empty()) {
      std::cout << " - " << entry.description;
    }
    if (entry.name == current) {
      std::cout << " [current]";
    }
    std::cout << "\n";
  }
}

void PrintConversationList(const std::vector<pu::Conversation>& convs) {
  if (convs.empty()) {
    std::cout << "No saved conversations.\n";
    return;
  }
  for (const auto& c : convs) {
    std::cout << "  " << c.id << " (" << c.messages.size() << " messages) created: " << c.created_at << "\n";
  }
}

void PrintChatHelp() {
  std::cout << "Available commands:\n"
            << "  /help           Show this help\n"
            << "  /exit, /quit    Exit interactive mode\n"
            << "  /clear          Clear conversation history and agent lock\n"
            << "  /agent <name>   Switch to different agent\n"
            << "  /agents         List available agents\n"
            << "  /save [name] [--no-summary]  Save conversation and optionally generate summary\n"
            << "  /load <id>      Load a saved conversation\n"
            << "  /list           List saved conversations\n"
            << "  /export <id>    Export conversation to Markdown\n"
            << "  /note add <text>  Add a note for current agent\n"
            << "  /note show      Show notes for current agent\n"
            << "  /reload-tools   Reload external Python tools\n";
}

} // namespace

int RunAsk(int argc, char* argv[]) {
  std::string requested_expert;
  std::string prompt;
  bool show_reasoning = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cerr << "Usage: pu ask [--agent <name>] [--show-reasoning] <prompt>\n"
                << "Options:\n"
                << "  --agent <name>          Specify the agent to use\n"
                << "  --show-reasoning        Show model's internal reasoning\n"
                << "  -h, --help              Show this help message\n";
      return 0;
    } else if (arg == "--agent") {
      if (i + 1 < argc) requested_expert = argv[++i];
      else { std::cerr << "Error: --agent requires an argument\n"; return 1; }
    } else if (arg == "--show-reasoning") {
      show_reasoning = true;
    } else if (prompt.empty()) {
      prompt = arg;
    } else {
      std::cerr << "Error: unexpected argument '" << arg << "'\n";
      return 1;
    }
  }

  if (prompt.empty()) {
    std::cerr << "Error: prompt is required\n";
    return 1;
  }

  auto ctx = SetupAppContext(requested_expert, show_reasoning);
  try {
    ctx.manager.Dispatch(prompt);
  } catch (const std::exception& e) {
    std::cerr << "\nError: " << e.what() << "\n";
    return 1;
  }
  return 0;
}

int RunChat(int argc, char* argv[]) {
  std::string initial_agent;
  bool show_reasoning = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: pu chat [--agent <name>] [--show-reasoning]\n";
      return 0;
    } else if (arg == "--agent") {
      if (i + 1 < argc) {
        initial_agent = argv[++i];
      } else {
        std::cerr << "Error: --agent requires an argument\n";
        return 1;
      }
    } else if (arg == "--show-reasoning") {
      show_reasoning = true;
    } else {
      std::cerr << "Error: unexpected argument '" << arg << "'\n";
      return 1;
    }
  }

  auto ctx = SetupAppContext(initial_agent, show_reasoning);
  const auto& config = ctx.config;
  auto& manager = ctx.manager;
  std::string current_name = manager.GetActiveAgent();

  const char* home = std::getenv("HOME");
  auto pu_dir = std::filesystem::path(home ? home : ".") / ".pu";
  auto store_dir = pu_dir / "conversations";
  pu::ConversationStore store(store_dir);

  std::vector<pu::ChatMessage> panel_messages;
  int message_id = 0;

  struct ConfirmationState {
    bool auto_approve_safe = false;
    bool deny_all = false;
  };
  auto confirm_state = std::make_shared<ConfirmationState>();

  manager.SetConfirmationCallback([confirm_state](const pu::agent::ConfirmationRequest& req) {
    if (confirm_state->deny_all) return pu::agent::ConfirmationChoice::kDenyAll;
    if (confirm_state->auto_approve_safe && req.highest_risk == pu::executor::RiskLevel::kSafe) {
      return pu::agent::ConfirmationChoice::kApproveOnce;
    }

    std::cout << "[CONFIRM] " << req.description << " [y/N/a(all safe)/s(deny all)] ";
    std::string answer;
    std::getline(std::cin, answer);
    if (answer == "a") {
      confirm_state->auto_approve_safe = true;
      return (req.highest_risk == pu::executor::RiskLevel::kSafe)
                 ? pu::agent::ConfirmationChoice::kApproveOnce
                 : pu::agent::ConfirmationChoice::kDeny;
    }
    if (answer == "s") {
      confirm_state->deny_all = true;
      return pu::agent::ConfirmationChoice::kDenyAll;
    }
    return (answer == "y" || answer == "Y") ? pu::agent::ConfirmationChoice::kApproveOnce
                                            : pu::agent::ConfirmationChoice::kDeny;
  });

  auto global_ctx = pu::GlobalContext::Create(pu_dir);
  global_ctx->Load();

  auto call_stack = std::make_shared<pu::CallStack>();
  manager.SetGlobalContext(global_ctx);
  manager.SetCallStack(call_stack);
  pu::Orchestrator orchestrator(global_ctx, call_stack, manager);

  for (const auto& entry : config.experts) {
    auto summary_opt = global_ctx->Read("memory/summaries/" + entry.name + "/latest");
    if (summary_opt && summary_opt->is_string()) {
      std::string summary = summary_opt->get<std::string>();
      if (!summary.empty()) {
        manager.SetSystemPrompt(entry.name, summary);
      }
    }
  }

  std::cout << "[INFO] Connected to agent: " << current_name;
  const auto* entry_ptr = [&]() -> const pu::config::AgentEntry* {
    for (const auto& e : config.experts) {
      if (e.name == current_name) return &e;
    }
    return nullptr;
  }();
  if (entry_ptr && !entry_ptr->description.empty()) {
    std::cout << " (" << entry_ptr->description << ")";
  }
  std::cout << "\nType /help for available commands.\n\n";

  std::string input;
  while (std::cout << "> " << std::flush, std::getline(std::cin, input)) {
    if (input.empty()) {
      continue;
    }

    if (input[0] == '/') {
      std::string cmd_output;
      if (orchestrator.HandleCommand(input, cmd_output)) {
        std::cout << cmd_output << "\n";
        continue;
      }
      if (input == "/help") {
        PrintChatHelp();
      } else if (input == "/exit" || input == "/quit") {
        break;
      } else if (input == "/clear") {
        manager.ClearSessions();
        panel_messages.clear();
        message_id = 0;
        confirm_state->auto_approve_safe = false;
        confirm_state->deny_all = false;
        std::cout << "[INFO] Conversation history and agent lock cleared.\n";
      } else if (input == "/agents") {
        PrintAgents(config, manager.GetActiveAgent());
      } else if (input.rfind("/agent ", 0) == 0) {
        std::string new_name = Trim(input.substr(7));
        if (new_name.empty()) {
          std::cerr << "Error: agent name required.\n";
          continue;
        }

        const pu::config::AgentEntry* new_entry = nullptr;
        for (const auto& entry : config.experts) {
          if (entry.name == new_name) {
            new_entry = &entry;
            break;
          }
        }
        if (!new_entry) {
          std::cerr << "Error: agent '" << new_name << "' not found.\n";
          continue;
        }

        manager.SetActiveAgent(new_entry->name);
        current_name = new_name;
        std::cout << "[INFO] Switched to agent: " << new_entry->name;
        if (!new_entry->description.empty()) {
          std::cout << " (" << new_entry->description << ")";
        }
        std::cout << "\n";
      } else if (input.rfind("/save", 0) == 0) {
        std::string args = input.substr(5);
        bool no_summary = false;
        if (args.find("--no-summary") != std::string::npos) {
          no_summary = true;
          size_t pos = args.find("--no-summary");
          args.erase(pos, 13);
        }
        std::string save_name = Trim(args);
        if (save_name.empty()) {
          save_name = GenerateId();
        }

        pu::Conversation conv;
        conv.id = save_name;
        conv.created_at = panel_messages.empty() ? CurrentTimestamp() : panel_messages.front().timestamp;
        conv.updated_at = CurrentTimestamp();
        conv.messages = panel_messages;
        conv.expert_histories = manager.SnapshotAgents();

        std::error_code ec;
        store.Save(conv, ec);
        if (ec) {
          std::cerr << "Error: failed to save conversation: " << ec.message() << "\n";
          continue;
        }
        std::cout << "[INFO] Conversation saved as '" << conv.id << "'\n";

        if (!no_summary) {
          std::ostringstream summary_prompt;
          summary_prompt << "Summarize the main tasks and decisions you handled in this session. Be concise (max 10 lines).\n\n";
          for (const auto& msg : panel_messages) {
            if (msg.role == "user" || msg.role == current_name) {
              summary_prompt << "[" << msg.role << "]: " << msg.content << "\n";
            }
          }

          try {
            auto summary = manager.CallAgent(current_name, summary_prompt.str());
            if (!summary.empty()) {
              std::cout << "\n[Memory] Generated summary for '" << current_name << "':\n"
                        << summary << "\n\n"
                        << "Accept this summary? [y/N] or enter additional text: ";
              std::string user_input;
              std::getline(std::cin, user_input);
              if (user_input == "y" || user_input == "Y") {
                global_ctx->Write("memory/summaries/" + current_name + "/latest", summary);
                std::cout << "[Memory] Summary saved.\n";
              } else if (!user_input.empty() && user_input != "n" && user_input != "N") {
                auto final_summary = summary + "\n\nUser notes: " + user_input;
                global_ctx->Write("memory/summaries/" + current_name + "/latest", final_summary);
                std::cout << "[Memory] Updated summary saved.\n";
              }
            }
          } catch (const std::exception& e) {
            std::cerr << "Error generating summary: " << e.what() << "\n";
          }
        }
      } else if (input.rfind("/load ", 0) == 0) {
        std::string load_id = Trim(input.substr(6));
        if (load_id.empty()) {
          std::cerr << "Error: conversation id required.\n";
          continue;
        }

        std::error_code ec;
        auto conv = store.Load(load_id, ec);
        if (ec) {
          std::cerr << "Error: failed to load conversation: " << ec.message() << "\n";
        } else {
          panel_messages = conv.messages;
          message_id = panel_messages.empty() ? 0 : panel_messages.back().id;
          manager.RestoreAgents(conv.expert_histories);
          manager.SetActiveAgent("");
          confirm_state->auto_approve_safe = false;
          confirm_state->deny_all = false;
          std::cout << "[INFO] Loaded conversation '" << load_id << "'\n";
        }
      } else if (input == "/list") {
        auto convs = store.List();
        PrintConversationList(convs);
      } else if (input.rfind("/export ", 0) == 0) {
        std::string export_id = Trim(input.substr(8));
        if (export_id.empty()) {
          std::cerr << "Error: conversation id required.\n";
          continue;
        }

        std::error_code ec;
        auto md = store.ExportMarkdown(export_id, ec);
        if (ec) {
          std::cerr << "Error: failed to export conversation: " << ec.message() << "\n";
          continue;
        }
        std::string filename = "conversation_" + export_id + ".md";
        std::ofstream out(filename);
        if (!out) {
          std::cerr << "Error: cannot write to " << filename << "\n";
        } else {
          out << md;
          std::cout << "[INFO] Exported to " << filename << "\n";
        }
      } else if (input.rfind("/note", 0) == 0) {
        if (input == "/note show") {
          auto notes_opt = global_ctx->Read("memory/notes/" + current_name);
          if (notes_opt && notes_opt->is_array()) {
            std::cout << "Notes for " << current_name << ":\n";
            for (const auto& note : *notes_opt) {
              if (note.is_string()) {
                std::cout << note.get<std::string>() << "\n";
              }
            }
          } else {
            std::cout << "No notes yet.\n";
          }
        } else if (input.rfind("/note add ", 0) == 0) {
          auto text = Trim(input.substr(10));
          if (text.empty()) {
            std::cerr << "Note text required.\n";
            continue;
          }
          std::string timestamped_note = "[" + CurrentTimestamp() + "] " + text;
          auto notes_opt = global_ctx->Read("memory/notes/" + current_name);
          json notes_array = json::array();
          if (notes_opt && notes_opt->is_array()) {
            notes_array = *notes_opt;
          }
          notes_array.push_back(timestamped_note);
          global_ctx->Write("memory/notes/" + current_name, notes_array);
          std::cout << "Note added.\n";
        } else {
          std::cerr << "Usage: /note add <text> | /note show\n";
        }
      } else if (input == "/reload-tools") {
        auto* agent = manager.GetAgent(manager.GetActiveAgent());
        if (agent) {
          auto* llm_agent = dynamic_cast<agents::LLMAgent*>(agent);
          if (llm_agent) {
            llm_agent->ReloadExternalTools();
            std::cout << "[INFO] External tools reloaded.\n";
          } else {
            std::cout << "[INFO] Current agent does not support tool reload.\n";
          }
        } else {
          std::cout << "[INFO] No active agent.\n";
        }
      } else {
        std::cerr << "Unknown command: " << input << "\nType /help for available commands.\n";
      }
      continue;
    }

    std::string actual_agent;
    if (!call_stack->IsEmpty()) {
      actual_agent = call_stack->Top().agent_name;
    } else {
      actual_agent = manager.GetActiveAgent();
      if (actual_agent.empty()) actual_agent = "chat";
    }

    panel_messages.push_back({++message_id, CurrentTimestamp(), "user", input, ""});
    manager.NotifyPanelMessage(panel_messages.back());

    try {
      std::string response = orchestrator.Process(input);
      if (!response.empty()) {
        panel_messages.push_back({++message_id, CurrentTimestamp(), actual_agent, response, ""});
        manager.NotifyPanelMessage(panel_messages.back());

        std::vector<pu::ChatMessage> recent;
        size_t start_idx = panel_messages.size() > 20 ? panel_messages.size() - 20 : 0;
        for (size_t i = start_idx; i < panel_messages.size(); ++i) {
          recent.push_back(panel_messages[i]);
        }
        manager.SetRecentMessages(recent);
        for (auto& [agent, text] : manager.CollectProactiveReplies()) {
          panel_messages.push_back({++message_id, CurrentTimestamp(), agent, text, ""});
          std::cout << "\n[" << agent << "] " << text << std::endl;
          manager.NotifyPanelMessage(panel_messages.back());
        }
      }
    } catch (const std::exception& e) {
      std::cerr << "\nError: " << e.what() << "\n\n";
    }
  }

  global_ctx->Save();
  std::cout << "\nGoodbye!\n";
  return 0;
}

int RunLearn(int argc, char* argv[]) {
  double threshold = 0.6;
  int max_sessions = 10;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cerr << "Usage: pu learn [--threshold <0.0-1.0>] [--max-sessions <N>]\n"
                << "  Analyze successful conversations and generate new agent definitions.\n"
                << "  Generated agents are saved to ~/.pu/generated/agents/\n";
      return 0;
    } else if (arg == "--threshold") {
      if (i + 1 < argc) {
        threshold = std::stod(argv[++i]);
      } else {
        std::cerr << "Error: --threshold requires a value\n";
        return 1;
      }
    } else if (arg == "--max-sessions") {
      if (i + 1 < argc) {
        max_sessions = std::stoi(argv[++i]);
      } else {
        std::cerr << "Error: --max-sessions requires a number\n";
        return 1;
      }
    } else {
      std::cerr << "Error: unknown argument '" << arg << "'\n";
      return 1;
    }
  }

  const char* home = std::getenv("HOME");
  auto pu_dir = std::filesystem::path(home ? home : ".") / ".pu";
  auto conv_dir = pu_dir / "conversations";
  auto generated_dir = pu_dir / "generated" / "agents";

  if (!std::filesystem::exists(conv_dir)) {
    std::cerr << "No conversations found in " << conv_dir << "\n";
    return 0;
  }

  std::filesystem::create_directories(generated_dir);

  // Placeholder: iterate conversations, analyze, generate agents.
  std::cout << "[Learn] Scanning " << conv_dir << " for sessions (threshold=" << threshold
            << ", max=" << max_sessions << ")\n";
  std::cout << "[Learn] Generated agents will be saved to " << generated_dir << "\n";
  std::cout << "[Learn] (Implementation in progress)\n";

  return 0;
}

} // namespace pu::cli
