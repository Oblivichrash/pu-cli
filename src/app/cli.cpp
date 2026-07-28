// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli.hpp"

#include "ui.hpp"

#include "pu/runtime/runtime.hpp"
#include "pu/session/workspace.hpp"
#include "pu/session/assignment.hpp"
#include "pu/session/call_stack.hpp"
#include "pu/agent_config.hpp"
#include "pu/path_utils.hpp"
#include "pu/conversation.hpp"
#include "pu/renderer.hpp"

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
  config::AgentsConfig agents_config;
  std::string config_path;
  std::string active_agent;
};

AppContext SetupAppContext(const std::string& requested_agent) {
  AppContext ctx;
  try {
    ctx.config_path = config::FindConfigPath();
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << '\n';
    std::exit(1);
  }

  ctx.agents_config = config::LoadAgentsConfig(ctx.config_path);

  if (ctx.agents_config.agents.empty()) {
    std::cerr << "Error: no agents configured\n";
    std::exit(1);
  }

  auto active_name = requested_agent.empty() ? ctx.agents_config.default_agent : requested_agent;
  bool active_found = false;

  for (const auto& entry : ctx.agents_config.agents) {
    if (entry.name == active_name) active_found = true;
  }

  if (!active_found) {
    std::cerr << "Error: agent '" << active_name << "' not found\nAvailable agents:\n";
    for (const auto& e : ctx.agents_config.agents) std::cerr << "  " << e.name << '\n';
    std::exit(1);
  }

  ctx.active_agent = active_name;
  return ctx;
}

}  // namespace

int RunAsk(int argc, char* argv[]) {
  std::string requested_agent;
  std::string prompt;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cerr << "Usage: pu ask [--agent <name>] <prompt>\n"
                << "Options:\n"
                << "  --agent <name>          Specify the agent to use\n"
                << "  -h, --help              Show this help message\n";
      return 0;
    } else if (arg == "--agent") {
      if (i + 1 < argc) requested_agent = argv[++i];
      else { std::cerr << "Error: --agent requires an argument\n"; return 1; }
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

  auto ctx = SetupAppContext(requested_agent);
  try {
    auto& runtime = Runtime::Instance();

    if (!requested_agent.empty()) {
      runtime.SetDefaultAgent(requested_agent);
    }

    runtime.Initialize();

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

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: pu chat [--agent <name>]\n";
      return 0;
    } else if (arg == "--agent") {
      if (i + 1 < argc) {
        initial_agent = argv[++i];
      } else {
        std::cerr << "Error: --agent requires an argument\n";
        return 1;
      }
    } else {
      std::cerr << "Error: unexpected argument '" << arg << "'\n";
      return 1;
    }
  }

  auto ctx = SetupAppContext(initial_agent);
  const auto& agents_config = ctx.agents_config;
  std::string current_name = ctx.active_agent;

  auto& runtime = Runtime::Instance();

  if (!initial_agent.empty()) {
    runtime.SetDefaultAgent(initial_agent);
  }

  runtime.Initialize();

  auto session = runtime.GetDefaultSession();
  if (!session) {
    std::cerr << "Error: could not create session\n";
    return 1;
  }
  auto session_id = session->GetId();

  std::cout << "[INFO] Connected to session: " << session_id << "\n";
  std::cout << "[INFO] Connected to agent: " << current_name;
  const auto* entry_ptr = [&]() -> const config::AgentEntry* {
    for (const auto& e : agents_config.agents) {
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
    if (input.empty()) continue;

    if (input == "/exit" || input == "/quit") {
      break;
    }

    std::string output;
    bool is_command = false;
    if (runtime.ProcessInput(session_id, input, output, is_command)) {
      if (!output.empty()) {
        std::cout << output << "\n";
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