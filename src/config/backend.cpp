// SPDX-License-Identifier: GPL-3.0-only
#include "pu/config/backend.hpp"

#include "pu/llm/capabilities.hpp"

#include "pu/config/gateway.hpp"
#include "pu/core/base.hpp"

#include <memory>
#include <utility>

namespace pu::config {

std::unique_ptr<pu::LLMProvider> CreateBackend(const BackendConfig& cfg,
                                               std::shared_ptr<pu::http::HttpClient> http) {
  if (cfg.host.empty()) {
    throw pu::Error("Missing host for backend type: " + std::string(BackendTypeName(cfg.type)));
  }
  return GatewayForType(cfg.type).make_provider(cfg, std::move(http));
}

std::vector<std::string> ListModels(const BackendConfig& cfg, pu::http::HttpClient& http) {
  if (cfg.host.empty()) {
    throw pu::Error("Missing host for backend type: " + std::string(BackendTypeName(cfg.type)));
  }

  const std::unique_ptr<pu::ModelLister> listing = GatewayForType(cfg.type).make_listing();
  std::vector<std::string> ids;
  for (const llm::ModelEntry& entry : listing->ListModels(cfg.host, cfg.api_key.value_or(""), http)) {
    ids.push_back(entry.id);
  }
  return ids;
}

}  // namespace pu::config
