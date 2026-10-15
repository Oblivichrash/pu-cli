// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>

#include "pu/config/gateway.hpp"
#include "pu/llm/openai_provider.hpp"

namespace pu::config {

struct BackendConfig;

// The OpenAI protocol's config, filled from a backend. Shared because a field added for
// one gate would otherwise be silently defaulted by the others; each gate's own mapping
// lives beside this one, in gateways.cpp.
OpenAIProvider::Config OpenAIConfig(const BackendConfig& cfg);

const Gateway& OllamaGateway();
const Gateway& OpenAIGateway();
const Gateway& CodeBuddyGateway();

}  // namespace pu::config
