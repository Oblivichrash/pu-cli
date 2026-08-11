// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

#include "pu/agent_config.hpp"

namespace pu::config_tools {

int ListAgents(const std::string& config_path, bool json_output);
int ShowAgent(const std::string& config_path, const std::string& name, bool json_output);
int AddAgent(const std::string& config_path, const std::string& name);
int RemoveAgent(const std::string& config_path, const std::string& name);
int RenameAgent(const std::string& config_path, const std::string& old_name,
                const std::string& new_name);
int SetDefaultAgent(const std::string& config_path, const std::string& name);

}  // namespace pu::config_tools