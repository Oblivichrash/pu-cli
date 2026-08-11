// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <map>
#include <string>

namespace pu::config_tools {

const std::map<std::string, std::string>& GetPromptTemplates();

}  // namespace pu::config_tools