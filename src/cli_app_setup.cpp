// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli_app_setup.hpp"
#include "pu/agent_config.hpp"
#include "pu/expert.hpp"
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
  ctx.config = config::LoadExpertsConfig(ctx.config_path, ec);
  if (ec) {
    std::cerr << "Error: failed to load config: " << ec.message() << "\n";
    std::exit(1);
  }
  if (ctx.config.experts.empty()) {
    std::cerr << "Error: no experts configured\n";
    std::exit(1);
  }

  auto active_name = requested_expert.empty() ? ctx.config.default_expert : requested_expert;
  bool active_found = false;

  for (const auto& entry : ctx.config.experts) {
    if (entry.name == active_name) active_found = true;
    try {
      ctx.manager.RegisterExpert(expert::AgentRegistry::Instance().CreateExpert(entry));
    } catch (const std::exception& e) {
      std::cerr << "Error: failed to create expert '" << entry.name << "': " << e.what() << "\n";
      std::exit(1);
    }
  }
  if (!active_found) {
    std::cerr << "Error: expert '" << active_name << "' not found\nAvailable experts:\n";
    for (const auto& e : ctx.config.experts) std::cerr << "  " << e.name << "\n";
    std::exit(1);
  }

  ctx.manager.SetActiveExpert(active_name);
  if (show_reasoning) ctx.manager.SetShowReasoning(true);
  return ctx;
}

}  // namespace pu::cli
