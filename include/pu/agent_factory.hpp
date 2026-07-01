// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/agent.hpp"
#include "pu/agent_config.hpp"
#include <memory>

namespace pu::agent {

class AgentRegistry {
 public:
  static AgentRegistry& Instance();
  std::unique_ptr<BaseAgent> CreateAgent(const config::AgentEntry& entry);

 private:
  AgentRegistry() = default;
};

}  // namespace pu::agent
