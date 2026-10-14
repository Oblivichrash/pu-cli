// SPDX-License-Identifier: GPL-3.0-only
#include "gateways.hpp"

#include "pu/config/backend.hpp"

#include "pu/llm/model_listing.hpp"
#include "pu/llm/openai_provider.hpp"

namespace pu::config {

namespace {

OpenAIProvider::Config OpenAIConfig(const BackendConfig& cfg) {
  OpenAIProvider::Config protocol;
  protocol.model = cfg.model;
  protocol.temperature = cfg.temperature;
  protocol.host = cfg.host;
  protocol.api_key = cfg.api_key.value_or("");
  protocol.max_tokens = cfg.max_tokens;
  protocol.thinking = cfg.thinking;
  return protocol;
}

std::unique_ptr<pu::LLMProvider> MakeOpenAIProvider(const BackendConfig& cfg,
                                                    std::shared_ptr<pu::http::HttpClient> http) {
  return std::make_unique<OpenAIProvider>(OpenAIConfig(cfg), std::move(http));
}

std::unique_ptr<pu::ModelLister> MakeOpenAIListing() {
  return std::make_unique<llm::OpenAIModelLister>();
}

}  // namespace

const Gateway& OpenAIGateway() {
  static const Gateway gateway{
      .id = GatewayId::kOpenAI,
      .name = "openai",
      .default_host = "https://api.openai.com/v1",
      .make_provider = &MakeOpenAIProvider,
      .make_listing = &MakeOpenAIListing,
  };
  return gateway;
}

}  // namespace pu::config
