// SPDX-License-Identifier: GPL-3.0-only
#include "pu/config/gateway.hpp"

#include "gateways.hpp"

#include <array>

namespace pu::config {

namespace {

const std::array<const Gateway*, 3> kGateways = {
    &OllamaGateway(),
    &OpenAIGateway(),
    &CodeBuddyGateway(),
};

}  // namespace

std::span<const Gateway* const> Gateways() { return kGateways; }

const Gateway* FindGateway(std::string_view name) {
  for (const Gateway* gateway : kGateways) {
    if (gateway->name == name) return gateway;
  }
  return nullptr;
}

const Gateway& GatewayForType(BackendType type) {
  for (const Gateway* gateway : kGateways) {
    if (gateway->type == type) return *gateway;
  }
  throw Error("No gate answers for backend type " + std::to_string(static_cast<int>(type)));
}

}  // namespace pu::config
