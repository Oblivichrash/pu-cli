// SPDX-License-Identifier: GPL-3.0-only
#include "pu/config/backend.hpp"

#include "pu/config/gateway.hpp"
#include "pu/llm/capabilities.hpp"
#include "pu/core/base.hpp"

#include <memory>
#include <utility>

namespace pu::config {

namespace {

// An empty host means the config named a host of "", not that the gate has none: a
// backend reaches here with its host already filled from the gate's default.
const Gateway& GateFor(const BackendConfig& cfg) {
  if (cfg.host.empty()) {
    throw pu::Error("Missing host for gateway: " + std::string(GatewayName(cfg.gateway)));
  }
  return GatewayForId(cfg.gateway);
}

}  // namespace

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
  return GateFor(cfg).make_provider(cfg, std::move(http));
}

std::vector<llm::ModelEntry> ListModels(const BackendConfig& cfg, pu::http::HttpClient& http) {
  const std::unique_ptr<pu::ModelLister> listing = GateFor(cfg).make_listing();
  return listing->ListModels(cfg.host, cfg.api_key.value_or(""), http);
}

}  // namespace pu::config
