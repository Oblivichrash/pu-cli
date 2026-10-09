// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// What a workspace's agents.json says: the agents it configures, what each may do, and
// where a `pu serve` started there should listen. Reading it into these types lives here too.

#include <optional>
#include <string>
#include <vector>

#include "pu/config/backend.hpp"
#include "pu/mcp/client.hpp"

namespace pu::config {

struct SecurityPolicy {
  std::string sandbox_root;
  size_t max_command_length = 0;
  std::vector<std::string> forbidden_patterns;
};

struct AgentEntry {
  std::string name;
  std::string description;
  BackendConfig backend;
  SecurityPolicy security;
  std::vector<pu::mcp::McpServerConfig> mcp_servers;
};

struct AgentsConfig {
  std::string default_agent;
  std::vector<AgentEntry> agents;
};

// Where a workspace says its server should listen: a directory is a session, so its port
// belongs beside its other facts rather than in whichever shell starts a server for it.
struct ServeOptions {
  std::optional<std::string> host;
  std::optional<int> port;
};

std::string FindConfigPath();

// The `serve` block of the workspace's configuration. A file that is absent, unreadable or
// malformed answers as if it had none: `LoadAgentsConfig` reports on it a moment later.
std::optional<ServeOptions> FindServeOptions();

AgentsConfig LoadAgentsConfig(const std::string& config_path);

}  // namespace pu::config
