// SPDX-License-Identifier: GPL-3.0-only
// Which gateway a standalone tool reads, resolved from the agent file rather than from a
// session. `pu-probe` and `pu-models` both ask this and nothing above it, so the rule that a
// named gateway wins over an agent's own is stated once. The `pu` command frame does not use
// it: there the answer is the running session's backend, which is a different question.
#pragma once

#include <string>

#include "pu/config/agents.hpp"
#include "pu/model/backend.hpp"

namespace pu::config {

// A named gateway is built from the agent's backend and never adopted. An unnamed one is the agent's
// own. `model` overrides that, which is the one thing a caller gets to say about a file it did
// not write.
model::Backend BackendFromAgent(const std::string& gateway, const std::string& model,
                                const std::string& agent, const config::AgentEntry& entry);

// The same, reading the agent file itself. Throws when the file names no such agent, since the
// caller asked for one by name and a silently different gateway would be the wrong answer to
// the wrong question.
model::Backend BackendFromAgent(const std::string& gateway, const std::string& model,
                                const std::string& agent, const std::string& config_path);

}  // namespace pu::config
