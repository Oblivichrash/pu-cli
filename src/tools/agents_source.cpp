// SPDX-License-Identifier: GPL-3.0-only
// The table an `--entry` report copies from, read without a session. Both `pu models` and the
// standalone `pu-models` go through here, so the two print the same entry for the same file.

#include "pu/tools/models.hpp"

#include "pu/config/agents.hpp"

namespace pu::tools {

AgentsSource ReadAgentsSource(const std::string& config_path, const std::string& agent_name) {
  AgentsSource source;
  source.config_path = config_path;
  source.agent_name = agent_name;
  // The raw value rather than the parsed entry: a parsed `api_key` is the expanded secret, and
  // an entry written from one would paste a key into the file.
  source.entry = config::FindRawAgentEntry(config_path, agent_name);
  return source;
}

}  // namespace pu::tools
