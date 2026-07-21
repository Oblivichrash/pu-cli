// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/agent_core.hpp"
#include <string>

namespace pu::agent {

class AgentExecutor {
 public:
  explicit AgentExecutor(AgentManager& manager);

  AgentContext PrepareContext(const std::string& agent_name);
  std::string Execute(const std::string& agent_name, const std::string& input, AgentContext& ctx);
  std::string Dispatch(const std::string& input);

 private:
  std::string CallAgent(const std::string& agent_name, const std::string& input);
  AgentManager& manager_;
};

}  // namespace pu::agent
