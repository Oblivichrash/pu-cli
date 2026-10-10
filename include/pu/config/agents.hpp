// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pu/config/backend.hpp"
#include "pu/config/mcp_server.hpp"
#include "pu/config/security_policy.hpp"

namespace pu::config {

struct AgentEntry {
  std::string name;
  std::string description;
  BackendConfig backend;
  SecurityPolicy security;
  std::vector<McpServerConfig> mcp_servers;
};

struct AgentsConfig {
  std::string default_agent;
  std::vector<AgentEntry> agents;
};

struct ServeOptions {
  std::optional<std::string> host;
  std::optional<std::uint16_t> port;
};

std::optional<std::uint16_t> ParsePort(const std::string& text);

struct ListenOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 8080;
};

ListenOptions ResolveListenOptions(const std::optional<std::string>& flag_host,
                                   const std::optional<std::string>& flag_port,
                                   const std::optional<std::string>& env_host,
                                   const std::optional<std::string>& env_port,
                                   const std::optional<ServeOptions>& from_file);

std::string FindConfigPath();

std::optional<ServeOptions> FindServeOptions();

AgentsConfig LoadAgentsConfig(const std::string& config_path);

}  // namespace pu::config
