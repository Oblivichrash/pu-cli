// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>

namespace pu::config { struct AgentEntry; }
namespace pu::agent { class BaseAgent; }

namespace pu::agent {

// Free factory function replacing the AgentRegistry singleton
std::unique_ptr<BaseAgent> CreateAgent(const config::AgentEntry& entry);

} // namespace pu::agent
