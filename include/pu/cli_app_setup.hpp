// SPDX-License-Identifier: GPL-3.0-only
//
// Shared CLI application context and initialization.

#pragma once

#include "pu/expert.hpp"
#include "pu/expert_config.hpp"

#include <string>

namespace pu::cli {

struct AppContext {
  config::ExpertsConfig config;
  expert::ExpertManager manager;
  std::string config_path;
};

// Setup the expert manager from configuration.
// If `requested_expert` is non‑empty, it will be set as the active expert
// provided it exists; otherwise the config default is used.
// `show_reasoning` enables reasoning token output.
AppContext SetupAppContext(const std::string& requested_expert = "",
                          bool show_reasoning = false);

}  // namespace pu::cli
