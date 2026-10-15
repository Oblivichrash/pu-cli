// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "pu/llm/model_listing.hpp"

namespace pu {

class LLMProvider;

namespace http {
class HttpClient;
}

}  // namespace pu

namespace pu::config {

struct Backend;

// A stored agents.json names one of these, and the spelling is what the file carries.
enum class GatewayId { kOllama, kOpenAI, kCodeBuddy };

std::string DefaultHostFor(GatewayId id);

const char* GatewayName(GatewayId id);
std::string GatewayNames();

std::optional<GatewayId> ParseGatewayId(std::string_view name);

// One gateway is one product, not one protocol: it names the vendor's endpoint, and it
// carries the product-level facts a protocol cannot know — the default host, the
// catalogue's shape, and how that product bills (one reports `credit`, another reports
// nothing at all). Two vendors that both speak OpenAI's wire format are still two
// gateways.
struct Gateway {
  GatewayId id;
  std::string_view name;
  std::string_view default_host;
  std::unique_ptr<pu::LLMProvider> (*make_provider)(const Backend&,
                                                    std::shared_ptr<pu::http::HttpClient>);
  std::unique_ptr<pu::ModelLister> (*make_listing)();
};

// Adding a gateway is a file beside the others plus a line in the gateway table in gateways.cpp.
std::span<const Gateway* const> Gateways();

const Gateway* FindGateway(std::string_view name);

const Gateway& GatewayForId(GatewayId id);

}  // namespace pu::config
