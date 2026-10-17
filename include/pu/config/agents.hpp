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
  Backend backend;
  SecurityPolicy security;
  std::vector<McpServerConfig> mcp_servers;
  // The tool-call budget for one turn. Absent: the executor's default applies.
  std::optional<int> tool_call_budget;
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

// Renders one agents.json entry for `model` from `source`, which is the file's text and not a
// parsed entry: a parsed `api_key` is the expanded secret. A reasoning-only model gets the
// catalogue's effort; a request carrying none is answered 400 `11150`.
std::string FormatAgentEntry(const llm::ModelEntry& model, const boost::json::value& source);

// The entry as the file spells it, before `${ENV}` expansion and before `host` is filled in.
std::optional<boost::json::value> FindRawAgentEntry(const std::string& config_path,
                                                    const std::string& agent_name);

std::string FindConfigPath();

std::optional<ServeOptions> FindServeOptions();

AgentsConfig LoadAgentsConfig(const std::string& config_path);

}  // namespace pu::config
