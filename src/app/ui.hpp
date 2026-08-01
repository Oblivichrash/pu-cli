// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

#include "pu/agent_config.hpp"

namespace pu::cli {

std::string Trim(const std::string& s);
std::string CurrentTimestamp();
std::string GenerateId();
void PrintAgents(const pu::config::AgentsConfig& cfg, const std::string& current);
void PrintChatHelp();

}  // namespace pu::cli
