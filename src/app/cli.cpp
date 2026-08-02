// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#include <spdlog/spdlog.h>

#include "pu/agent_config.hpp"
#include "pu/conversation.hpp"
#include "pu/error.hpp"
#include "pu/path_utils.hpp"
#include "pu/runtime/runtime.hpp"
#include "pu/session/assignment.hpp"
#include "pu/session/call_stack.hpp"
#include "pu/session/workspace.hpp"
#include "ui.hpp"

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
    spdlog::error("{}", e.what());
    std::exit(1);
  }

  ctx.agents_config = config::LoadAgentsConfig(ctx.config_path);

  if (ctx.agents_config.agents.empty()) {
    spdlog::error("no agents configured");
    std::exit(1);
  }

  auto active_name = requested_agent.empty() ? ctx.agents_config.default_agent : requested_agent;
  bool active_found = false;

  for (const auto& entry : ctx.agents_config.agents) {
    if (entry.name == active_name) active_found = true;
  }

  if (!active_found) {
    spdlog::error("agent '{}' not found", active_name);
    for (const auto& e : ctx.agents_config.agents) spdlog::info("  {}", e.name);
    std::exit(1);
  }

  ctx.active_agent = active_name;
  return ctx;
}

}  // namespace

int RunAsk(int argc, char* argv[], Runtime& runtime) {
  std::string requested_agent;
  std::string prompt;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: pu ask [--agent <name>] <prompt>\n"
                << "Options:\n"
                << "  --agent <name>          Specify the agent to use\n"
                << "  -h, --help              Show this help message\n";
      return 0;
    } else if (arg == "--agent") {
      if (i + 1 < argc) requested_agent = argv[++i];
      else {
        spdlog::error("--agent requires an argument");
        return 1;
      }
    } else if (prompt.empty()) {
      prompt = arg;
    } else {
      spdlog::error("unexpected argument '{}'", arg);
      return 1;
    }
  }

  if (prompt.empty()) {
    spdlog::error("prompt is required");
    return 1;
  }

  auto ctx = SetupAppContext(requested_agent);
  try {
    if (!requested_agent.empty()) {
      runtime.SetDefaultAgent(requested_agent);
    }

    runtime.Initialize();

    auto session = runtime.GetDefaultSession();
    if (!session) {
      spdlog::error("could not create session");
      return 1;
    }

    ExecutionResult result;
    bool is_command = false;
    if (runtime.ProcessInput(session->GetId(), prompt, result, is_command)) {
      if (result.has_error) {
        spdlog::error("{}", result.error_message);
      } else if (!result.content.empty()) {
        std::cout << result.content << "\n";
      } else if (!is_command) {
        std::cout << "\n";
      }
    } else {
      spdlog::error("{}", result.error_message.empty() ? "Request failed" : result.error_message);
    }

    runtime.Shutdown();
  } catch (const std::exception& e) {
    spdlog::error("{}", e.what());
    return 1;
  }
  return 0;
}

int RunChat(int argc, char* argv[], Runtime& runtime) {
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
        spdlog::error("--agent requires an argument");
        return 1;
      }
    } else {
      spdlog::error("unexpected argument '{}'", arg);
      return 1;
    }
  }

  auto ctx = SetupAppContext(initial_agent);
  const auto& agents_config = ctx.agents_config;
  std::string current_name = ctx.active_agent;

  if (!initial_agent.empty()) {
    runtime.SetDefaultAgent(initial_agent);
  }

  runtime.Initialize();

  auto session = runtime.GetDefaultSession();
  if (!session) {
    spdlog::error("could not create session");
    return 1;
  }
  auto session_id = session->GetId();

  spdlog::info("Connected to session: {}", session_id);
  std::string agent_info = "Connected to agent: " + current_name;
  const auto* entry_ptr = [&]() -> const config::AgentEntry* {
    for (const auto& e : agents_config.agents) {
      if (e.name == current_name) return &e;
    }
    return nullptr;
  }();
  if (entry_ptr && !entry_ptr->description.empty()) {
    agent_info += " (" + entry_ptr->description + ")";
  }
  spdlog::info("{}", agent_info);
  spdlog::info("Type /help for available commands.");

  std::string input;
  while (std::cout << "> " << std::flush, std::getline(std::cin, input)) {
    if (input.empty()) continue;

    if (input == "/exit" || input == "/quit") {
      break;
    }

    try {
      ExecutionResult result;
      bool is_command = false;
      if (runtime.ProcessInput(session_id, input, result, is_command)) {
        if (result.has_error) {
          spdlog::error("{}", result.error_message);
        } else if (!result.was_streamed) {
          if (!result.content.empty()) {
            std::cout << result.content << "\n";
          } else if (!is_command) {
            std::cout << "\n";
          }
        } else {
          if (!is_command) {
            std::cout << "\n";
          }
        }
      } else if (is_command && result.error_message.empty()) {
        std::cout << "Unknown command. ";
        PrintChatHelp();
      } else {
        spdlog::error("{}", result.error_message.empty() ? "Processing failed" : result.error_message);
      }
    } catch (const std::exception& e) {
      spdlog::error("{}", e.what());
    }
  }

  runtime.Shutdown();
  std::cout << "\nGoodbye!\n";
  return 0;
}

}  // namespace pu::cli
