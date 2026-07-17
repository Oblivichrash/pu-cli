// SPDX-License-Identifier: GPL-3.0-only
#include "pu/agent_factory.hpp"
#include "executor/command_executor.hpp"
#include "agents/llm/llm_agent.hpp"
#include "http/curl_http_client.hpp"
#include "pu/agent_config.hpp"
#include "pu/token_adapter.hpp"
#include "backends/ollama/ollama_token_adapter.hpp"
#include "backends/openai/openai_token_adapter.hpp"
#include "pu/tools/execute_bash_tool.hpp"
#include "pu/tools/write_file_tool.hpp"
#include "pu/tools/create_tool.hpp"
#include <memory>

namespace pu::agent {

AgentRegistry& AgentRegistry::Instance() {
  static AgentRegistry registry;
  return registry;
}

std::unique_ptr<BaseAgent> AgentRegistry::CreateAgent(const config::AgentEntry& entry) {
  auto http = std::make_unique<http::CurlHttpClient>();

  std::unique_ptr<backends::ITokenAdapter> adapter;
  switch (entry.backend.tool_call_style) {
    case config::ToolCallStyle::kOpenAI:
      adapter = std::make_unique<backends::openai::OpenAITokenAdapter>();
      break;
    default:
      adapter = std::make_unique<backends::ollama::OllamaTokenAdapter>();
      break;
  }

  auto backend = config::CreateBackend(entry.backend, std::move(http), std::move(adapter));

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
    }
    // External Python tools are auto-loaded, unknown names are ignored.
  }

  return std::make_unique<agents::LLMAgent>(entry.name, std::move(backend), std::move(tool_registry), entry.security);
}

}  // namespace pu::agent
