// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

#include "pu/http_client.hpp"

namespace pu::config_tools {

// No-arg overloads use a real CurlHttpClient; the overloads taking
// pu::http::HttpClient& exist so tests can inject mocks.
std::vector<std::string> scanNvidiaNIM();
std::vector<std::string> scanNvidiaNIM(pu::http::HttpClient& http);

std::vector<std::string> scanOllama();
std::vector<std::string> scanOllama(pu::http::HttpClient& http);

std::vector<std::string> scanOpenAICompatible(const std::string& host,
                                              const std::string& api_key);
std::vector<std::string> scanOpenAICompatible(const std::string& host,
                                              const std::string& api_key,
                                              pu::http::HttpClient& http);

}  // namespace pu::config_tools
