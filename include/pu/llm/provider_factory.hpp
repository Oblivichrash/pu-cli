// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>

#include "pu/agent_config.hpp"
#include "pu/llm/llm_provider.hpp"
#include "pu/http_client.hpp"

namespace pu::config {

// Single source of truth for constructing LLM providers from a BackendConfig.
// Session::CreateProvider and tests delegate here so provider construction
// never diverges between the chat UI and programmatic use.
std::unique_ptr<pu::LLMProvider> CreateBackend(
    const BackendConfig& cfg, std::unique_ptr<pu::http::HttpClient> http);

}  // namespace pu::config
