// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <vector>

#include "pu/config/mcp_server.hpp"
#include "pu/mcp/client.hpp"

namespace pu::mcp {

std::shared_ptr<McpClient> ConnectMcpServer(const config::McpServerConfig& config);

std::vector<std::shared_ptr<McpClient>> ConnectMcpServers(
    const std::vector<config::McpServerConfig>& configs);

void DisconnectMcpServers(std::vector<std::shared_ptr<McpClient>>& clients);

}  // namespace pu::mcp
