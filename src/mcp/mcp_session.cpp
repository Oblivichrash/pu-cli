// SPDX-License-Identifier: GPL-3.0-only
#include "pu/mcp/mcp_session.hpp"

#include <spdlog/spdlog.h>

namespace pu::mcp {

std::shared_ptr<McpClient> ConnectMcpServer(const config::McpServerConfig& config) {
  auto client = std::make_shared<McpClient>(config);
  if (client->Connect()) return client;

  spdlog::warn("MCP server '{}' connection failed", config.name);
  return nullptr;
}

std::vector<std::shared_ptr<McpClient>> ConnectMcpServers(
    const std::vector<config::McpServerConfig>& configs) {
  std::vector<std::shared_ptr<McpClient>> clients;
  clients.reserve(configs.size());

  for (const auto& config : configs) {
    if (auto client = ConnectMcpServer(config)) clients.push_back(std::move(client));
  }
  return clients;
}

void DisconnectMcpServers(std::vector<std::shared_ptr<McpClient>>& clients) {
  for (auto& client : clients) {
    if (client) client->Disconnect();
  }
  clients.clear();
}

}  // namespace pu::mcp
