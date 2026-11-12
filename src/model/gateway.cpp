// SPDX-License-Identifier: GPL-3.0-only
#include "pu/model/gateway.hpp"

#include "pu/model/backend.hpp"
#include "pu/core/text.hpp"

#include <string>
#include <string_view>

// Only the facts about a product live here: the name, the host, the protocol it speaks and how
// it bills. Which provider a row opens is `llm`'s answer, so the table of rows is written there
// and handed back through `InstallGatewayTable` — this file only reads it. That is what lets
// `model` be looked up from below `llm` while the factories it names stay above.
namespace pu::model {

std::string DefaultHostFor(GatewayId id) { return std::string(GatewayForId(id).default_host); }

std::string_view GatewayEndpointPath(GatewayId id) { return GatewayForId(id).endpoint_path; }

const char* GatewayName(GatewayId id) { return GatewayForId(id).name.data(); }

std::string GatewayNames() {
  return text::Join(Gateways(), ", ", [](const Gateway& gateway) { return gateway.name; });
}

std::optional<GatewayId> ParseGatewayId(std::string_view name) {
  const Gateway* gateway = FindGateway(name);
  if (gateway == nullptr) return std::nullopt;
  return gateway->id;
}

std::string_view ApiKeyEnvFor(GatewayId id) { return GatewayForId(id).api_key_env; }

}  // namespace pu::model
