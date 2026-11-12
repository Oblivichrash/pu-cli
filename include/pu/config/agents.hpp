// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "pu/model/backend.hpp"
#include "pu/config/mcp_server.hpp"
#include "pu/paging/paging.hpp"
#include "pu/config/security_policy.hpp"

namespace pu::config {

struct AgentEntry {
  std::string name;
  std::string description;
  model::Backend backend;
  SecurityPolicy security;
  paging::Budget paging;
  std::vector<McpServerConfig> mcp_servers;
  // The tool-call budget for one turn. Absent: the executor's default applies.
  std::optional<int> tool_call_budget;
  // Which agents this one may hand a turn to, by name. Empty means it may hand a turn to
  // nobody, which is the answer a file that says nothing should get: a table that does not
  // mention delegation cannot be used to reach an agent it does not mention.
  std::vector<std::string> delegates;
  // The builtin tools this one has, by name. Empty means every builtin, which is what a table
  // written before this field existed meant and what it must keep meaning.
  std::vector<std::string> tools;
  // Where this entry was read from: the file's directory, which is what a `persona` and an
  // `mcp_servers` path are relative to. Empty for an inline entry in the top-level table.
  std::filesystem::path source_dir;
};

struct AgentsConfig {
  std::string default_agent;
  std::vector<AgentEntry> agents;
};

// Where the page and the API listen. `bind` is the interface the listener sits on, not an address
// to dial: a backend has a `host` because it is reached, while this is the one place this program
// answers. `0.0.0.0` is a legal bind and never a legal destination, so the two are not the same
// word, and a page that reads one as the other is how a working server looks broken.
struct ServeOptions {
  std::optional<std::string> bind;
  std::optional<std::uint16_t> port;
};

std::optional<std::uint16_t> ParsePort(const std::string& text);

struct ListenOptions {
  std::string bind = "127.0.0.1";
  std::uint16_t port = 8080;
};

ListenOptions ResolveListenOptions(const std::optional<std::string>& flag_bind,
                                   const std::optional<std::string>& flag_port,
                                   const std::optional<std::string>& env_bind,
                                   const std::optional<std::string>& env_port,
                                   const std::optional<ServeOptions>& from_file);

// Renders one agents.json entry for `model` from `source`, which is the file's text and not a
// parsed entry: a parsed `api_key` is the expanded secret. A reasoning-only model gets the
// catalogue's effort; a request carrying none is answered 400 `11150`.
std::string FormatAgentEntry(const model::ModelEntry& model, const boost::json::value& source);

// The entry as the file spells it, before `${ENV}` expansion and before `host` is filled in.
std::optional<boost::json::value> FindRawAgentEntry(const std::string& config_path,
                                                    const std::string& agent_name);

std::string FindConfigPath();

// The directory an agent's own table lives in, beside the top-level `agents.json`: one
// directory per agent, named after it, holding that agent's table and whatever its table
// points at. An empty path when this build looks nowhere else, which it does not.
std::filesystem::path AgentTableDir(const std::filesystem::path& config_path);

// Every builtin tool a `tools` list may name. One list, so a name that `pu check` accepts is
// one the runtime can register.
const std::vector<std::string>& BuiltinToolNames();

std::optional<ServeOptions> FindServeOptions();

AgentsConfig LoadAgentsConfig(const std::string& config_path);

}  // namespace pu::config
