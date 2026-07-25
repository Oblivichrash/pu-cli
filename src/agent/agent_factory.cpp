// SPDX-License-Identifier: GPL-3.0-only
#include "pu/agent_core.hpp"

#include "agent/llm_agent.hpp"
#include "tools/command_executor.hpp"
#include "infra/curl_http_client.hpp"

#include "pu/tools/create_tool.hpp"
#include "pu/tools/execute_bash_tool.hpp"
#include "pu/tools/write_file_tool.hpp"
#include "pu/tools/fork_tools.hpp"

#include <memory>

namespace pu::agent {

AgentRegistry& AgentRegistry::Instance() {
  static AgentRegistry registry;
  return registry;
}

std::unique_ptr<BaseAgent> AgentRegistry::CreateAgent(const config::AgentEntry& entry) {
  auto http = std::make_unique<http::CurlHttpClient>();
  auto backend = config::CreateBackend(entry.backend, std::move(http));

  auto tool_registry = std::make_unique<ToolRegistry>();

  for (const auto& tool_name : entry.tools) {
    if (tool_name == "execute_bash") {
      std::string sandbox = entry.security.sandbox_root.empty() ? "." : entry.security.sandbox_root;
      auto executor = std::make_unique<executor::CommandExecutor>(sandbox);
      tool_registry->RegisterTool(std::make_unique<tools::ExecuteBashToolStandard>(std::move(executor)));
    } else if (tool_name == "create_tool") {
      tool_registry->RegisterTool(std::make_unique<tools::CreateTool>());
    } else if (tool_name == "write_file") {
      tool_registry->RegisterTool(std::make_unique<tools::WriteFileTool>());
    } else if (tool_name == "fork_context") {
      tool_registry->RegisterTool(std::make_unique<tools::ForkContextTool>());
    } else if (tool_name == "merge_context") {
      tool_registry->RegisterTool(std::make_unique<tools::MergeContextTool>());
    } else if (tool_name == "list_forks") {
      tool_registry->RegisterTool(std::make_unique<tools::ListForksTool>());
    }
  }

  return std::make_unique<agents::LLMAgent>(entry.name, std::move(backend), std::move(tool_registry), entry.security);
}

}  // namespace pu::agent
