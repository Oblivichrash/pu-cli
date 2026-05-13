// SPDX-License-Identifier: GPL-3.0-only
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

AppContext SetupAppContext(const std::string& requested_expert = "",
                          bool show_reasoning = false);

}  // namespace pu::cli
