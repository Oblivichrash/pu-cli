// SPDX-License-Identifier: GPL-3.0-only
#include "common.hpp"
#include "pu/agent_factory.hpp"
#include "pu/agent_config.hpp"
#include <cstdlib>
#include <iostream>

namespace pu::cli {

AppContext SetupAppContext(const std::string& requested_agent, bool show_reasoning) {
  AppContext ctx;
  try {
    ctx.config_path = config::FindConfigPath();
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    std::exit(1);
  }

  try {
    ctx.config = config::LoadAgentsConfig(ctx.config_path);
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to load config: " << e.what() << "\n";
    std::exit(1);
  }

  if (ctx.config.agents.empty()) {
    std::cerr << "Error: no agents configured\n";
    std::exit(1);
  }

  auto active_name = requested_agent.empty() ? ctx.config.default_agent : requested_agent;
  bool active_found = false;

  for (const auto& entry : ctx.config.agents) {
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
    for (const auto& e : ctx.config.agents) std::cerr << "  " << e.name << "\n";
    std::exit(1);
  }

  ctx.manager.SetActiveAgent(active_name);
  if (show_reasoning) ctx.manager.SetShowReasoning(true);
  return ctx;
}

void PrintAgents(const config::AgentsConfig& config, const std::string& current) {
  std::cout << "Available agents:\n";
  for (const auto& entry : config.agents) {
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

void PrintConversationList(const std::vector<Conversation>& convs) {
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

} // namespace pu::cli
