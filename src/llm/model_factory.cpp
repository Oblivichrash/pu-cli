// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/model_factory.hpp"

#include "pu/llm/llm_provider.hpp"
#include "pu/llm/model_listing.hpp"

#include <memory>
#include <utility>

namespace pu::llm {

std::unique_ptr<pu::LLMProvider> CreateBackend(const model::Backend& cfg,
                                               std::shared_ptr<pu::http::HttpClient> http) {
  return model::GatewayForConfig(cfg).make_provider(cfg, std::move(http));
}

std::vector<model::ModelEntry> ListModels(const model::Backend& cfg, pu::http::HttpClient& http) {
  const std::unique_ptr<pu::ModelLister> listing = model::GatewayForConfig(cfg).make_listing();
  return listing->ListModels(cfg.host, cfg.api_key.value_or(""), http);
}

}  // namespace pu::llm
