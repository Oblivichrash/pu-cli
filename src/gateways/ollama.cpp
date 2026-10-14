// SPDX-License-Identifier: GPL-3.0-only
#include "gateways.hpp"

#include "pu/llm/model_listing.hpp"
#include "pu/llm/ollama_provider.hpp"

namespace pu::config {

namespace {

std::unique_ptr<pu::LLMProvider> MakeOllamaProvider(const BackendConfig& cfg,
                                                    std::shared_ptr<pu::http::HttpClient> http) {
  OllamaProvider::Config protocol;
  protocol.model = cfg.model;
  protocol.temperature = cfg.temperature;
  protocol.host = cfg.host;
  protocol.api_key = cfg.api_key.value_or("");
  protocol.thinking = cfg.thinking;
  return std::make_unique<OllamaProvider>(std::move(protocol), std::move(http));
}

std::unique_ptr<pu::ModelLister> MakeOllamaListing() {
  return std::make_unique<llm::OllamaModelLister>();
}

}  // namespace

const Gateway& OllamaGateway() {
  static const Gateway gateway{
      .type = BackendType::kOllama,
      .name = "ollama",
      .default_host = "http://localhost:11434",
      .make_provider = &MakeOllamaProvider,
      .make_listing = &MakeOllamaListing,
  };
  return gateway;
}

}  // namespace pu::config
