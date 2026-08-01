// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace pu::mcp {

struct McpServerConfig {
    std::string name;
    std::string command;
    std::vector<std::string> args;
};

} // namespace pu::mcp
