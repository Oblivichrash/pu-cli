// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "pu/llm/model_lister.hpp"

namespace pu {

class LLMProvider;

namespace http {
class HttpClient;
}

}  // namespace pu

namespace pu::config {

struct BackendConfig;

// A stored agents.json names one of these, and the spelling is what the file carries.
enum class GatewayId { kOllama, kOpenAI, kCodeBuddy };

std::string DefaultHostFor(GatewayId id);

const char* GatewayName(GatewayId id);
std::string GatewayNames();

std::optional<GatewayId> ParseGatewayId(std::string_view name);

struct Gateway {
  GatewayId id;
  std::string_view name;
  std::string_view default_host;
  std::unique_ptr<pu::LLMProvider> (*make_provider)(const BackendConfig&,
                                                    std::shared_ptr<pu::http::HttpClient>);
  std::unique_ptr<pu::ModelLister> (*make_listing)();
};

// Adding a gate is a file beside the others plus a line in registry.cpp.
std::span<const Gateway* const> Gateways();

const Gateway* FindGateway(std::string_view name);

const Gateway& GatewayForId(GatewayId id);

}  // namespace pu::config
