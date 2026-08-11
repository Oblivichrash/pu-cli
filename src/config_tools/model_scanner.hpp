// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

#include "pu/http_client.hpp"
#include "config_tools/provider_registry.hpp"

namespace pu::config_tools {

// No-arg overload uses a real CurlHttpClient; the overload taking
// pu::http::HttpClient& exists so tests can inject mocks.
std::vector<std::string> scanProvider(const ProviderConfig& config);
std::vector<std::string> scanProvider(const ProviderConfig& config,
                                      pu::http::HttpClient& http);

}  // namespace pu::config_tools
