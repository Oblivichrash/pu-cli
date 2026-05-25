// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/agent.hpp"
#include "pu/backend.hpp"
#include "pu/agent_config.hpp"
#include <memory>
#include <unordered_map>

namespace pu::agent {

class AgentFactory {
 public:
  virtual ~AgentFactory() = default;
  virtual std::unique_ptr<BaseAgent> Create(
      const config::AgentEntry& entry,
      std::unique_ptr<backend::Backend> backend) = 0;
};

class AgentRegistry {
 public:
  static AgentRegistry& Instance();
  void RegisterFactory(config::AgentType type, std::unique_ptr<AgentFactory> factory);
  std::unique_ptr<BaseAgent> CreateExpert(const config::AgentEntry& entry);

 private:
  AgentRegistry() = default;
  std::unordered_map<config::AgentType, std::unique_ptr<AgentFactory>> factories_;
};

void RegisterBuiltinFactories();

}  // namespace pu::agent
