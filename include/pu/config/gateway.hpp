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

// The gates a build knows, by id. A stored agents.json names one, so the spelling is
// what a file carries and what this maps onto a gate.
enum class GatewayId { kOllama, kOpenAI, kCodeBuddy };

// The host a config may leave out, answered by the gate rather than spelled again here.
std::string DefaultHostFor(GatewayId id);

// The name a gate answers to, and all of them in one line.
const char* GatewayName(GatewayId id);
std::string GatewayNames();

// The gate a name refers to, or nothing when no gate answers to it.
std::optional<GatewayId> ParseGatewayId(std::string_view name);

// One gate: the name it answers to, the host a config may leave out, and how to build its
// provider and its catalogue read. What a gate does differently lives in its own file.
struct Gateway {
  GatewayId id;
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

// The gate an id names. The id is what an agents.json entry carries, so it keeps its
// spelling and resolves through this.
const Gateway& GatewayForId(GatewayId id);

}  // namespace pu::config
