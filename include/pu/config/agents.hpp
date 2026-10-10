// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// What a workspace's agents.json says: the agents it configures, what each may do, and
// where a `pu serve` started there should listen. Reading it into these types lives here too.

#include <cstdint>
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
  std::optional<std::uint16_t> port;
};

// The port a text names, or nothing when it does not name one: 1 to 65535, digits only.
std::optional<std::uint16_t> ParsePort(const std::string& text);

// Where a server should listen, once every place that may say so has answered: the command
// line, then the environment, then the workspace's serve block, then the defaults. A place
// that named a port keeps it even when it is refused, so a file cannot overrule it.
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

// The `serve` block of the workspace's configuration. A file that is absent, unreadable or
// malformed answers as if it had none: `LoadAgentsConfig` reports on it a moment later.
std::optional<ServeOptions> FindServeOptions();

AgentsConfig LoadAgentsConfig(const std::string& config_path);

}  // namespace pu::config
