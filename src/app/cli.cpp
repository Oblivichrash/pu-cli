// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli.hpp"

#include "infra/curl_http_client.hpp"
#include "session.hpp"
#include "ui.hpp"

#include "pu/runtime/runtime.hpp"
#include "pu/session/workspace.hpp"
#include "pu/session/assignment.hpp"
#include "pu/session/call_stack.hpp"
#include "pu/path_utils.hpp"
#include "tools/command_executor.hpp"

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

  // In Phase 3, agents are managed via metadata in the config.
  // BaseAgent instances are not created here - the Runtime manages sessions.
  for (const auto& entry : ctx.agents_config.agents) {
    if (entry.name == active_name) active_found = true;
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
    auto& runtime = Runtime::Instance();
    runtime.Initialize();
    
    // Create a session and process the prompt
    auto session = runtime.GetDefaultSession();
    if (!session) {
      std::cerr << "Error: could not create session\n";
      return 1;
    }
    
    std::string output;
    bool is_command = false;
    runtime.ProcessInput(session->GetId(), prompt, output, is_command);
    std::cout << output << "\n";
    
    runtime.Shutdown();
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

  auto& runtime = Runtime::Instance();
  runtime.Initialize();

  // Get default session
  auto session = runtime.GetDefaultSession();
  if (!session) {
    std::cerr << "Error: could not create session\n";
    return 1;
  }
  auto session_id = session->GetId();

  std::cout << "[INFO] Connected to session: " << session_id << "\n";

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

    if (input == "/exit" || input == "/quit") {
      break;
    }

    // Use Runtime to process all input (commands and messages)
    std::string output;
    bool is_command = false;
    if (runtime.ProcessInput(session_id, input, output, is_command)) {
      if (!output.empty()) {
        std::cout << output << "\n";
      }
      
      // For regular messages, update panel messages
      if (!is_command && !output.empty()) {
        panel_messages.push_back({++message_id, CurrentTimestamp(), "user", input, ""});
        panel_messages.push_back({++message_id, CurrentTimestamp(), current_name, output, ""});
      }
    } else {
      std::cerr << "Error: " << output << "\n";
    }
  }

  runtime.Shutdown();
  std::cout << "\nGoodbye!\n";
  return 0;
}

}  // namespace pu::cli
