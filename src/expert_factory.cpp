// SPDX-License-Identifier: GPL-3.0-only

#include "pu/expert_factory.hpp"

#include "executor/command_executor.hpp"
#include "experts/chat/chat_expert.hpp"
#include "experts/bash/bash_expert.hpp"
#include "http/curl_http_client.hpp"
#include "pu/expert_config.hpp"

#include <memory>
#include <system_error>

namespace pu::expert {

class ChatExpertFactory : public ExpertFactory {
  std::unique_ptr<BaseExpert> Create(const config::ExpertEntry& entry,
                                     std::unique_ptr<backend::Backend> backend) override {
    return std::make_unique<experts::ChatExpert>(entry.name,
                                                 std::move(backend),
                                                 entry.name);
  }
};

class BashExpertFactory : public ExpertFactory {
  std::unique_ptr<BaseExpert> Create(const config::ExpertEntry& entry,
                                     std::unique_ptr<backend::Backend> backend) override {
    auto executor = std::make_unique<executor::CommandExecutor>(entry.sandbox_path);
    return std::make_unique<experts::BashExpert>(entry.name,
                                                 std::move(backend),
                                                 std::move(executor),
                                                 entry.confirmation_policy);
  }
};

ExpertRegistry& ExpertRegistry::Instance() {
  static ExpertRegistry registry;
  return registry;
}

void ExpertRegistry::RegisterFactory(config::ExpertType type,
                                     std::unique_ptr<ExpertFactory> factory) {
  factories_[type] = std::move(factory);
}

std::unique_ptr<BaseExpert> ExpertRegistry::CreateExpert(const config::ExpertEntry& entry) {
  auto http = std::make_unique<http::CurlHttpClient>();

  std::error_code ec;
  auto backend = config::CreateBackend(entry.backend, std::move(http), ec);
  if (ec) {
    throw std::runtime_error("Failed to create backend for expert '" + entry.name + "': " + ec.message());
  }

  auto it = factories_.find(entry.type);
  if (it == factories_.end()) {
    throw std::runtime_error("No factory registered for expert type");
  }

  return it->second->Create(entry, std::move(backend));
}

void RegisterBuiltinFactories() {
  ExpertRegistry::Instance().RegisterFactory(
      config::ExpertType::kChat,
      std::make_unique<ChatExpertFactory>());
  ExpertRegistry::Instance().RegisterFactory(
      config::ExpertType::kBash,
      std::make_unique<BashExpertFactory>());
}

}  // namespace pu::expert
