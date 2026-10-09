// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// What a workspace's agents.json says: the agents it configures, what each of them may
// do, and where a `pu serve` started in that workspace should listen. Reading that file
// into these types is declared here too, since the file and the shapes it fills are the
// same subject.

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

// Where a workspace says its own server should listen. A directory is a session, and
// several directories are meant to be served side by side, so the port belongs beside
// the directory's other facts rather than in whichever shell happens to start a
// server for it.
struct ServeOptions {
  std::optional<std::string> host;
  std::optional<int> port;
};

std::string FindConfigPath();

// The `serve` block of the workspace's configuration, when it has one. A file that is
// absent, unreadable or malformed answers as if it had none: `LoadAgentsConfig`
// reports on that a moment later, in words about the file rather than about a port.
std::optional<ServeOptions> FindServeOptions();

AgentsConfig LoadAgentsConfig(const std::string& config_path);

}  // namespace pu::config
