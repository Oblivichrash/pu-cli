// SPDX-License-Identifier: GPL-3.0-only

#include "pu/cli_app_setup.hpp"
#include "pu/expert_config.hpp"
#include "pu/expert.hpp"
#include "pu/expert_factory.hpp"

#include <iostream>

namespace pu::cli {

AppContext SetupAppContext(const std::string& requested_expert,
                          bool show_reasoning) {
  AppContext ctx;

  try {
    ctx.config_path = config::FindConfigPath();
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    std::exit(1);
  }

  try {
    ctx.config = config::LoadExpertsConfig(ctx.config_path);
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to load config: " << e.what() << "\n";
    std::exit(1);
  }

  if (ctx.config.experts.empty()) {
    std::cerr << "Error: no experts configured\n";
    std::exit(1);
  }

  std::string active_name = requested_expert.empty()
                                ? ctx.config.default_expert
                                : requested_expert;
  bool active_found = false;

  for (const auto& entry : ctx.config.experts) {
    if (entry.name == active_name) {
      active_found = true;
    }

    try {
      auto expert = expert::ExpertRegistry::Instance().CreateExpert(entry);
      ctx.manager.RegisterExpert(std::move(expert));
    } catch (const std::exception& e) {
      std::cerr << "Error: failed to create expert '" << entry.name << "': " << e.what() << "\n";
      std::exit(1);
    }
  }

  if (!active_found) {
    std::cerr << "Error: expert '" << active_name << "' not found\n";
    std::cerr << "Available experts:\n";
    for (const auto& e : ctx.config.experts) {
      std::cerr << "  " << e.name << "\n";
    }
    std::exit(1);
  }

  ctx.manager.SetActiveExpert(active_name);
  if (show_reasoning) {
    ctx.manager.SetShowReasoning(true);
  }

  return ctx;
}

}  // namespace pu::cli
