// SPDX-License-Identifier: GPL-3.0-only
#include "pu/model/backend.hpp"

#include "pu/model/gateway.hpp"
#include "pu/core/base.hpp"

#include <cstdlib>
#include <memory>
#include <utility>

namespace pu::model {

// The table's own accessors -- `DefaultHostFor`, `GatewayName`, `ParseGatewayId` and the rest --
// are defined once, in gateway.cpp, which is the file that exists for them. They were defined
// here as well, identically, and GNU ld silently picked one of the two; MSVC reports LNK2005
// instead.

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
