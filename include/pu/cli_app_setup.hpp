// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/agent.hpp"
#include "pu/agent_config.hpp"
#include <string>

namespace pu::cli {

struct AppContext {
  config::AgentsConfig config;
  agent::AgentManager manager;
  std::string config_path;
};

AppContext SetupAppContext(const std::string& requested_expert = "",
                          bool show_reasoning = false);

}  // namespace pu::cli
