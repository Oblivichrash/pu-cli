// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <span>
#include <string_view>

#include "pu/config/backend.hpp"
#include "pu/llm/model_lister.hpp"

namespace pu::config {

// One gate: the name it answers to, the host a config may leave out, and how to build its
// provider and its catalogue read. What a gate does differently lives in its own file.
struct Gateway {
  BackendType type;
  std::string_view name;
  std::string_view default_host;
  std::unique_ptr<pu::LLMProvider> (*make_provider)(const BackendConfig&,
                                                    std::shared_ptr<pu::http::HttpClient>);
  std::unique_ptr<pu::ModelLister> (*make_listing)();
};

// The gates this build knows. Adding one is a file beside the others plus a line in
// registry.cpp; removing one is the same in reverse.
std::span<const Gateway* const> Gateways();

// The gate that answers to `name`, or nullptr when none does.
const Gateway* FindGateway(std::string_view name);

// The gate a backend type maps onto. The type is what a stored session and an agents.json
// entry carry, so it keeps its spelling and resolves through this.
const Gateway& GatewayForType(BackendType type);

}  // namespace pu::config
