// SPDX-License-Identifier: GPL-3.0-only
#include "pu/model/backend.hpp"

#include "pu/model/gateway.hpp"
#include "pu/core/base.hpp"
#include "pu/core/text.hpp"

#include <cstdlib>
#include <memory>
#include <ranges>
#include <utility>

namespace pu::model {

std::string DefaultHostFor(GatewayId type) {
  return std::string(GatewayForId(type).default_host);
}

std::string_view GatewayEndpointPath(GatewayId type) {
  return GatewayForId(type).endpoint_path;
}

const char* GatewayName(GatewayId type) { return GatewayForId(type).name.data(); }

std::string GatewayNames() {
  return text::Join(
      std::views::transform(Gateways(), [](const Gateway& gateway) { return gateway.name; }));
}

std::string_view ApiKeyEnvFor(GatewayId id) { return GatewayForId(id).api_key_env; }

std::optional<GatewayId> ParseGatewayId(std::string_view name) {
  if (const Gateway* gateway = FindGateway(name); gateway != nullptr) return gateway->id;
  return std::nullopt;
}

Backend BackendForGateway(std::string_view gateway) {
  const std::optional<GatewayId> id = ParseGatewayId(gateway);
  if (!id) {
    throw pu::GenericError("Unknown gateway: " + std::string(gateway) + "; name one of " +
                           GatewayNames());
  }
  Backend cfg;
  cfg.gateway = *id;
  cfg.host = DefaultHostFor(*id);
  const std::string_view env = ApiKeyEnvFor(*id);
  if (!env.empty()) {
    const std::string name(env);
    if (const char* value = std::getenv(name.c_str()); value != nullptr && *value != '\0') {
      cfg.api_key = std::string(value);
    }
  }
  return cfg;
}

}  // namespace pu::model
