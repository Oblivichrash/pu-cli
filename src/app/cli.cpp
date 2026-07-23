// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli.hpp"

#include "agent/llm_agent.hpp"
#include "infra/curl_http_client.hpp"
#include "session.hpp"
#include "ui.hpp"

#include "pu/core/context.hpp"
#include "pu/core/delegation.hpp"
#include "pu/core/delegation_stack.hpp"
#include "pu/executor.hpp"
#include "pu/orchestrator.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

namespace pu::cli {

namespace {

struct AppContext {
  agent::config::AgentsConfig agents_config;
  agent::AgentManager manager;
  std::string config_path;
};

AppContext SetupAppContext(const std::string& requested_agent, bool show_reasoning) {
  AppContext ctx;
  try {
    ctx.config_path = agent::config::FindConfigPath();
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << '\n';
    std::exit(1);
  }

  ctx.agents_config = agent::config::LoadAgentsConfig(ctx.config_path);

  if (ctx.agents_config.agents.empty()) {
    std::cerr << "Error: no agents configured\n";
    std::exit(1);
  }

  auto active_name = requested_agent.empty() ? ctx.agents_config.default_agent : requested_agent;
  bool active_found = false;

  for (const auto& entry : ctx.agents_config.agents) {
    if (entry.name == active_name) active_found = true;
    try {
      ctx.manager.RegisterAgent(agent::AgentRegistry::Instance().CreateAgent(entry));
    } catch (const std::exception& e) {
      std::cerr << "Error: failed to create agent '" << entry.name << "': " << e.what() << '\n';
      std::exit(1);
    }
  }

  if (!active_found) {
    std::cerr << "Error: agent '" << active_name << "' not found\nAvailable agents:\n";
    for (const auto& e : ctx.agents_config.agents) std::cerr << "  " << e.name << '\n';
    std::exit(1);
  }

  ctx.manager.SetActiveAgent(active_name);
  if (show_reasoning) ctx.manager.SetShowReasoning(true);
  return ctx;
}

}  // namespace

int RunAsk(int argc, char* argv[]) {
  std::string requested_agent;
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
      if (i + 1 < argc) requested_agent = argv[++i];
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

  auto ctx = SetupAppContext(requested_agent, show_reasoning);
  try {
    agent::AgentExecutor executor(ctx.manager);
    executor.Dispatch(prompt);
  } catch (const std::exception& e) {
    std::cerr << "\nError: " << e.what() << '\n';
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
  const auto& agents_config = ctx.agents_config;
  auto& manager = ctx.manager;
  std::string current_name = manager.GetActiveAgent();

  const char* home = std::getenv("HOME");
  auto pu_dir = std::filesystem::path(home ? home : ".") / ".pu";
  auto store_dir = pu_dir / "conversations";

  auto global_ctx = GlobalContext::Create(pu_dir);
  global_ctx->Load();

  auto root_context_path = pu_dir / "contexts" / "active" / "root.json";
  std::filesystem::create_directories(root_context_path.parent_path());
  auto root_context = core::Context::LoadOrCreate(root_context_path);
  auto delegation_stack = std::make_shared<core::DelegationStack>(root_context);

  manager.SetGlobalContext(global_ctx);

  SessionManager session(store_dir, manager);
  Orchestrator orchestrator(global_ctx, manager);
  orchestrator.SetDelegationStack(delegation_stack);

  agent::AgentExecutor executor(manager);
  executor.SetRootContext(root_context);

  for (const auto& entry : agents_config.agents) {
    auto summary_opt = global_ctx->Read("memory/summaries/" + entry.name + "/latest");
    if (summary_opt && summary_opt->is_string()) {
      std::string summary = summary_opt->get<std::string>();
      if (!summary.empty()) {
        manager.SetSystemPrompt(entry.name, summary);
      }
    }
  }

  struct ConfirmationState {
    bool auto_approve_safe = false;
    bool deny_all = false;
  };
  auto confirm_state = std::make_shared<ConfirmationState>();

  manager.SetConfirmationCallback([confirm_state](const agent::ConfirmationRequest& req) {
    if (confirm_state->deny_all) return agent::ConfirmationChoice::kDenyAll;
    if (confirm_state->auto_approve_safe && req.highest_risk == executor::RiskLevel::kSafe) {
      return agent::ConfirmationChoice::kApproveOnce;
    }

    std::cout << "[CONFIRM] " << req.description << " [y/N/a(all safe)/s(deny all)] ";
    std::string answer;
    std::getline(std::cin, answer);
    if (answer == "a") {
      confirm_state->auto_approve_safe = true;
      return (req.highest_risk == executor::RiskLevel::kSafe)
                 ? agent::ConfirmationChoice::kApproveOnce
                 : agent::ConfirmationChoice::kDeny;
    }
    if (answer == "s") {
      confirm_state->deny_all = true;
      return agent::ConfirmationChoice::kDenyAll;
    }
    return (answer == "y" || answer == "Y") ? agent::ConfirmationChoice::kApproveOnce
                                            : agent::ConfirmationChoice::kDeny;
  });

  std::cout << "[INFO] Connected to agent: " << current_name;
  const auto* entry_ptr = [&]() -> const agent::config::AgentEntry* {
    for (const auto& e : agents_config.agents) {
      if (e.name == current_name) return &e;
    }
    return nullptr;
  }();
  if (entry_ptr && !entry_ptr->description.empty()) {
    std::cout << " (" << entry_ptr->description << ")";
  }
  std::cout << "\nType /help for available commands.\n\n";

  std::vector<ChatMessage> panel_messages;
  int message_id = 0;

  std::string input;
  while (std::cout << "> " << std::flush, std::getline(std::cin, input)) {
    if (input.empty()) continue;

    if (input[0] == '/') {
      std::string cmd_output;
      if (orchestrator.HandleCommand(input, cmd_output)) {
        std::cout << cmd_output << '\n';
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
        PrintAgents(agents_config, manager.GetActiveAgent());
      } else if (input.rfind("/agent ", 0) == 0) {
        std::string new_name = Trim(input.substr(7));
        if (new_name.empty()) {
          std::cerr << "Error: agent name required.\n";
          continue;
        }

        const agent::config::AgentEntry* new_entry = nullptr;
        for (const auto& entry : agents_config.agents) {
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
        std::cout << '\n';
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

        session.SaveConversation(save_name, panel_messages, no_summary, *global_ctx, root_context);

        if (!no_summary) {
          std::ostringstream summary_prompt;
          summary_prompt << "Summarize the main tasks and decisions you handled in this session. Be concise (max 10 lines).\n\n";
          for (const auto& msg : panel_messages) {
            if (msg.role == "user" || msg.role == current_name) {
              summary_prompt << "[" << msg.role << "]: " << msg.content << "\n";
            }
          }

          try {
            agent::AgentContext exec_ctx = executor.PrepareContext(current_name);
            auto summary = executor.Execute(current_name, summary_prompt.str(), exec_ctx);
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
            std::cerr << "Error generating summary: " << e.what() << '\n';
          }
        }
      } else if (input.rfind("/load ", 0) == 0) {
        std::string load_id = Trim(input.substr(6));
        if (load_id.empty()) {
          std::cerr << "Error: conversation id required.\n";
          continue;
        }

        session.LoadConversation(load_id, panel_messages);
        message_id = panel_messages.empty() ? 0 : panel_messages.back().id;
        confirm_state->auto_approve_safe = false;
        confirm_state->deny_all = false;
      } else if (input == "/list") {
        auto convs = session.ListConversations();
        PrintConversationList(convs);
      } else if (input.rfind("/export ", 0) == 0) {
        std::string export_id = Trim(input.substr(8));
        if (export_id.empty()) {
          std::cerr << "Error: conversation id required.\n";
          continue;
        }

        auto md = session.ExportMarkdown(export_id);
        if (!md.empty()) {
          std::string filename = "conversation_" + export_id + ".md";
          std::ofstream out(filename);
          if (!out) {
            std::cerr << "Error: cannot write to " << filename << '\n';
          } else {
            out << md;
            std::cout << "[INFO] Exported to " << filename << '\n';
          }
        }
      } else if (input.rfind("/note", 0) == 0) {
        if (input == "/note show") {
          auto notes = session.ShowNotes(current_name, *global_ctx);
          if (notes.empty()) {
            std::cout << "No notes yet.\n";
          } else {
            std::cout << "Notes for " << current_name << ":\n";
            for (const auto& note : notes) {
              std::cout << note << '\n';
            }
          }
        } else if (input.rfind("/note add ", 0) == 0) {
          auto text = Trim(input.substr(10));
          if (text.empty()) {
            std::cerr << "Note text required.\n";
            continue;
          }
          session.AddNote(current_name, text, *global_ctx);
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

    std::string actual_agent = manager.GetActiveAgent();
    if (actual_agent.empty()) actual_agent = "chat";

    panel_messages.push_back({++message_id, CurrentTimestamp(), "user", input, ""});

    try {
      std::string response = orchestrator.Process(input);
      if (!response.empty()) {
        panel_messages.push_back({++message_id, CurrentTimestamp(), actual_agent, response, ""});
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
    std::cerr << "No conversations found in " << conv_dir << '\n';
    return 0;
  }

  std::filesystem::create_directories(generated_dir);

  std::cout << "[Learn] Scanning " << conv_dir << " for sessions (threshold=" << threshold
            << ", max=" << max_sessions << ")\n";
  std::cout << "[Learn] Generated agents will be saved to " << generated_dir << '\n';
  std::cout << "[Learn] (Implementation in progress)\n";

  return 0;
}

}  // namespace pu::cli
