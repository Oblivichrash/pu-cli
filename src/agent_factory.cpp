// SPDX-License-Identifier: GPL-3.0-only
#include "pu/agent_factory.hpp"

#include "executor/command_executor.hpp"
#include "agents/chat/chat_agent.hpp"
#include "agents/bash/bash_agent.hpp"
#include "http/curl_http_client.hpp"
#include "pu/agent_config.hpp"
#include "pu/token_adapter.hpp"
#include "backends/ollama/ollama_token_adapter.hpp"
#include "backends/openai/openai_token_adapter.hpp"

#include <memory>
#include <system_error>

namespace pu::expert {

namespace {
class ChatAgentFactory : public AgentFactory {
  std::unique_ptr<BaseAgent> Create(const config::AgentEntry& entry,
                                    std::unique_ptr<backend::Backend> backend) override {
    return std::make_unique<experts::ChatAgent>(entry.name, std::move(backend), entry.name);
  }
};

class BashAgentFactory : public AgentFactory {
  std::unique_ptr<BaseAgent> Create(const config::AgentEntry& entry,
                                    std::unique_ptr<backend::Backend> backend) override {
    auto executor = std::make_unique<executor::CommandExecutor>(entry.sandbox_path);
    return std::make_unique<experts::BashAgent>(entry.name, std::move(backend),
                                                std::move(executor),
                                                entry.confirmation_policy);
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

std::unique_ptr<BaseAgent> AgentRegistry::CreateExpert(const config::AgentEntry& entry) {
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
  if (it == factories_.end()) throw std::runtime_error("No factory registered for expert type");
  return it->second->Create(entry, std::move(backend));
}

void RegisterBuiltinFactories() {
  AgentRegistry::Instance().RegisterFactory(config::AgentType::kChat,
                                            std::make_unique<ChatAgentFactory>());
  AgentRegistry::Instance().RegisterFactory(config::AgentType::kBash,
                                            std::make_unique<BashAgentFactory>());
}

}  // namespace pu::expert
