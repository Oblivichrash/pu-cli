// SPDX-License-Identifier: GPL-3.0-only
#include "pu/agent_core.hpp"

#include "infra/curl_http_client.hpp"

#include "pu/tools/create_tool.hpp"
#include "pu/tools/execute_bash_tool.hpp"
#include "pu/tools/write_file_tool.hpp"
#include "pu/tools/fork_tools.hpp"
#include "pu/tools/toolbox.hpp"

#include <memory>

namespace pu::agent {

AgentRegistry& AgentRegistry::Instance() {
  static AgentRegistry registry;
  return registry;
}

std::unique_ptr<BaseAgent> AgentRegistry::CreateAgent(const config::AgentEntry& entry) {
  // TODO: Phase 2 - Create Executor with Toolbox instead of LLMAgent
  // For now, return nullptr; this will be updated when the CLI is refactored
  return nullptr;
}

}  // namespace pu::agent
