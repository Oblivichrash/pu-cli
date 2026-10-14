// SPDX-License-Identifier: GPL-3.0-only
#include "pu/config/backend.hpp"

#include "pu/config/gateway.hpp"
#include "pu/llm/capabilities.hpp"
#include "pu/core/base.hpp"

#include <memory>
#include <utility>

namespace pu::config {

std::string DefaultHostFor(GatewayId type) {
  return std::string(GatewayForId(type).default_host);
}

const char* GatewayName(GatewayId type) { return GatewayForId(type).name.data(); }

std::string GatewayNames() {
  std::string names;
  for (const Gateway* gateway : Gateways()) {
    if (!names.empty()) names += ", ";
    names += gateway->name;
  }
  return names;
}

std::optional<GatewayId> ParseGatewayId(std::string_view name) {
  if (const Gateway* gateway = FindGateway(name); gateway != nullptr) return gateway->id;
  return std::nullopt;
}

std::unique_ptr<pu::LLMProvider> CreateBackend(const BackendConfig& cfg,
                                               std::shared_ptr<pu::http::HttpClient> http) {
  if (cfg.host.empty()) {
    throw pu::Error("Missing host for backend type: " + std::string(GatewayName(cfg.gateway)));
  }
  return GatewayForId(cfg.gateway).make_provider(cfg, std::move(http));
}

std::vector<std::string> ListModels(const BackendConfig& cfg, pu::http::HttpClient& http) {
  if (cfg.host.empty()) {
    throw pu::Error("Missing host for backend type: " + std::string(GatewayName(cfg.gateway)));
  }

  const std::unique_ptr<pu::ModelLister> listing = GatewayForId(cfg.gateway).make_listing();
  std::vector<std::string> ids;
  for (const llm::ModelEntry& entry : listing->ListModels(cfg.host, cfg.api_key.value_or(""), http)) {
    ids.push_back(entry.id);
  }
  return ids;
}

}  // namespace pu::config
