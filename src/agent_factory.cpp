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
#include "pu/tools/execute_bash_tool_simple.hpp"
#include "pu/tools/write_file_tool.hpp"
#include <memory>
#include <system_error>

namespace pu::agent {

namespace {

class LLMAgentFactory : public AgentFactory {
  std::unique_ptr<BaseAgent> Create(const config::AgentEntry& entry,
                                    std::unique_ptr<backend::Backend> backend) override {
    auto tool_registry = std::make_unique<ToolRegistry>();

    for (const auto& tool_name : entry.tools) {
      std::string variant = "standard";
      auto it = entry.tool_variants.find(tool_name);
      if (it != entry.tool_variants.end()) {
        variant = it->second;
      }

      if (tool_name == "execute_bash") {
        auto executor = std::make_unique<executor::CommandExecutor>(entry.sandbox_path);
        if (variant == "simple") {
          tool_registry->RegisterTool(std::make_unique<tools::ExecuteBashToolSimple>(std::move(executor)));
        } else {
          tool_registry->RegisterTool(std::make_unique<tools::ExecuteBashToolStandard>(std::move(executor)));
        }
      } else if (tool_name == "create_tool") {
        tool_registry->RegisterTool(std::make_unique<tools::CreateTool>());
      } else if (tool_name == "write_file") {
        tool_registry->RegisterTool(std::make_unique<tools::WriteFileTool>());
      }
    }

    return std::make_unique<agents::LLMAgent>(entry.name, std::move(backend), std::move(tool_registry), entry.security);
  }
};

}  // namespace

AgentRegistry& AgentRegistry::Instance() {
  static AgentRegistry registry;
  return registry;
}

void AgentRegistry::RegisterFactory(config::AgentType type,
                                    std::unique_ptr<AgentFactory> factory) {
  factories_[type] = std::move(factory);
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
  std::error_code ec;
  auto backend = config::CreateBackend(entry.backend, std::move(http), std::move(adapter), ec);
  if (ec) throw std::runtime_error("Failed to create backend: " + ec.message());
  auto it = factories_.find(entry.type);
  if (it == factories_.end()) throw std::runtime_error("No factory registered for agent type");
  return it->second->Create(entry, std::move(backend));
}

void RegisterBuiltinFactories() {
  AgentRegistry::Instance().RegisterFactory(config::AgentType::kChat,
                                            std::make_unique<LLMAgentFactory>());
  AgentRegistry::Instance().RegisterFactory(config::AgentType::kBash,
                                            std::make_unique<LLMAgentFactory>());
}

}  // namespace pu::agent
