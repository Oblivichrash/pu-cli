// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pu/llm/model_listing.hpp"

namespace pu {

class LLMProvider;

namespace http {
class HttpClient;
}

}  // namespace pu

namespace pu::config {

struct Backend;

// A stored agents.json names one of these, and the spelling is what the file carries. One
// value is one product: `openai` and `deepseek` speak the same protocol and are still two
// entries, because a host is a property of the product rather than of the caller.
enum class GatewayId { kOllama, kOllamaCloud, kOpenAI, kDeepSeek, kNim, kCodeBuddy };

std::string DefaultHostFor(GatewayId id);

// The path a chat turn is posted to, from the gateway table.
std::string_view GatewayEndpointPath(GatewayId id);

const char* GatewayName(GatewayId id);
std::string GatewayNames();

std::optional<GatewayId> ParseGatewayId(std::string_view name);

// The variable a gateway's key is read from when the config names none, empty for a gateway
// that needs no key.
std::string_view ApiKeyEnvFor(GatewayId id);

// How a product prices a call. A product fact, not a protocol fact: `deepseek` and
// `codebuddy` speak the same wire and only one of them reports a price.
enum class BillingKind {
  // The product says nothing about what a call costs. Absent is not free: a gateway that
  // prices nothing and one that prices a call at zero are different answers, and only this
  // value can tell them apart.
  kNone,
  // The product reports what this call cost, in its own unit. The number is kept as sent;
  // comparing two products' numbers would be comparing two different currencies.
  kReported,
};

struct Billing {
  BillingKind kind = BillingKind::kNone;
  // The field under `usage` that carries it. Empty for a gateway that reports none.
  std::string_view field;
  // What the number is in, as the product names it: "credits" today. No conversion is done
  // with it — it exists so a reader can tell one product's unit from another's.
  std::string_view unit;
};

// One gateway is one product, not one protocol: it names the vendor's endpoint, and it
// carries the product-level facts a protocol cannot know — the default host, the
// catalogue's shape, and how that product bills (one reports `credit`, another reports
// nothing at all). Two vendors that both speak OpenAI's wire format are still two
// gateways.
struct Gateway {
  GatewayId id;
  std::string_view name;
  std::string_view default_host;
  // The environment variable a turn reads its key from when the config names none, empty for
  // a gateway that needs no key. A probe of one gateway is then one word on the command line.
  std::string_view api_key_env;
  // The wire format this product speaks: "openai" or "ollama". Two products sharing a
  // protocol share its factories; they differ in host, headers and catalogue.
  std::string_view protocol;
  // The path a chat turn is posted to. One fact per gateway, so a caller that posts
  // without a provider (the probe) reads it here rather than restating the providers'.
  std::string_view endpoint_path;
  std::unique_ptr<pu::LLMProvider> (*make_provider)(const Backend&,
                                                    std::shared_ptr<pu::http::HttpClient>);
  std::unique_ptr<pu::ModelLister> (*make_listing)();
  // The headers a listing read carries beyond the bearer token, empty for a gateway that
  // needs none. A probe reads the same endpoint as the lister, so it reads them here rather
  // than restating them.
  std::vector<std::string> (*listing_headers)();
  // How a call's price arrives, or that none does. A gateway that prices a call fills this
  // in; one that says nothing leaves it `kNone`.
  Billing billing{};
};

// Adding a gateway is a line in the gateway table in gateways.cpp. There is no file per
// gateway: the table holds every one, which is what keeps a gateway's name, host and
// provider in one place instead of one place each.
std::span<const Gateway> Gateways();

const Gateway* FindGateway(std::string_view name);

const Gateway& GatewayForId(GatewayId id);

}  // namespace pu::config
