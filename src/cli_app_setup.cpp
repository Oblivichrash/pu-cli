// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli_app_setup.hpp"
#include "pu/agent_config.hpp"
#include "pu/agent.hpp"
#include "pu/agent_factory.hpp"
#include <iostream>

namespace pu::cli {

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

  // Set default system prompt for BashAgent to encourage using write_file
  // This only applies if an agent named "bash" exists
  ctx.manager.SetSystemPrompt("bash",
      "You have a tool 'write_file' that can write text content to a file. "
      "Prefer using 'write_file' over bash commands like 'cat >', 'echo >', or 'tee' for writing files. "
      "Only use 'execute_bash' for commands that cannot be done by 'write_file'.");

  ctx.manager.SetActiveAgent(active_name);
  if (show_reasoning) ctx.manager.SetShowReasoning(true);
  return ctx;
}

}  // namespace pu::cli
