// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
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
  // nobody, which is the answer a file that says nothing should get: a file that does not
  // mention delegation cannot be used to reach an agent it does not mention.
  std::vector<std::string> delegates;
  // The builtin tools this one has, by name. Empty means every builtin, which is what the
  // field's absence has always meant and must keep meaning.
  std::vector<std::string> tools;
  // Where this entry was read from: the file's directory, which is what a `persona` and an
  // `mcp_servers` path are relative to. Empty for an entry written inline in the file itself.
  std::filesystem::path source_dir;
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

// Points the one agent this file describes at another gateway, model and host, in place, and
// leaves every other key byte-for-byte as written. It edits the file's own text rather than
// re-rendering a parsed entry: a parsed backend holds the expanded `api_key`, so writing that
// back would move a secret out of the environment and into the file, and an unset optional field
// would be written back as its default. Only `gateway`, `model`, `host` and the sampling
// settings below change.
//
// `host` is written as given and is never expanded, so a value spelled `${VAR}` stays four
// characters and a variable sign rather than becoming this run's address; the next load is where
// it is read. An empty `host` removes the key, which is what makes the gateway's own default
// apply again -- writing "" would pin an address that resolves to nothing.
//
// The sampling settings are written as given and removed when absent: a cleared `max_tokens` is
// the key gone, which is what "send no cap" looks like in the file, rather than a key holding a
// number nobody chose. `temperature` is always written, because it is not optional: a backend
// that never stated one still answers at some temperature, and the file is where that value is
// kept.
//
// The write is atomic: the new text goes to a sibling temporary file, is parsed back to prove it
// is still one agent file, and only then replaces the original. A failure leaves the file as it
// was and throws.
void UpdateAgentBackend(const std::string& config_path, const std::string& gateway_name,
                        const std::string& model_name, const std::string& host,
                        float temperature, const std::optional<int>& max_tokens,
                        const std::optional<float>& top_p, const std::optional<int>& seed,
                        const std::optional<int>& context_window);

// Every builtin tool a `tools` list may name. One list, so a name that `pu check` accepts is
// one the runtime can register.
const std::vector<std::string>& BuiltinToolNames();

// The keys an agents.json entry has, split the way the file splits them. These live beside the
// parsers rather than beside the command that reports on them: a list kept anywhere else is a
// second account of what the parser reads, and the two drift the first time a field is added —
// the parser learns it and the list does not, so a file that uses it is told the field is never
// read. `ParseAgentEntry` and `pu check` both ask these, which is what keeps them one answer.
//
// A key here is one the parser looks at, not one this build acts on: `max_tokens` is read and
// carried even though no limit is enforced, so it is not reported as unknown.
const std::set<std::string>& ReadAgentKeys();
const std::set<std::string>& ReadBackendKeys();
const std::set<std::string>& ReadSecurityKeys();
const std::set<std::string>& ReadPagingKeys();
const std::set<std::string>& ReadMcpServerKeys();

std::optional<ServeOptions> FindServeOptions();

// A config file describes exactly one agent, and this is it. One file, one agent.
AgentEntry LoadAgentConfig(const std::string& config_path);

}  // namespace pu::config
