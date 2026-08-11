// SPDX-License-Identifier: GPL-3.0-only
#include "model_scanner.hpp"

#include <cstdlib>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pu/error.hpp"
#include "infra/curl_http_client.hpp"

namespace pu::config_tools {

namespace {

constexpr const char* kNvidiaModelsUrl = "https://integrate.api.nvidia.com/v1/models";
constexpr const char* kOllamaTagsUrl = "http://localhost:11434/api/tags";

// OpenAI-style endpoints (NIM, OpenAI-compatible) list models under data[].id.
std::vector<std::string> ParseOpenAIModels(const std::string& body) {
  std::vector<std::string> ids;
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(body);
  } catch (const std::exception&) {
    return ids;
  }
  if (!j.contains("data") || !j["data"].is_array()) return ids;
  for (const auto& item : j["data"]) {
    if (item.contains("id") && item["id"].is_string()) {
      ids.push_back(item["id"].get<std::string>());
    }
  }
  return ids;
}

std::vector<std::string> ParseOllamaModels(const std::string& body) {
  std::vector<std::string> ids;
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(body);
  } catch (const std::exception&) {
    return ids;
  }
  if (!j.contains("models") || !j["models"].is_array()) return ids;
  for (const auto& item : j["models"]) {
    if (item.contains("name") && item["name"].is_string()) {
      ids.push_back(item["name"].get<std::string>());
    }
  }
  return ids;
}

}  // namespace

std::vector<std::string> scanNvidiaNIM() {
  pu::http::CurlHttpClient http;
  return scanNvidiaNIM(http);
}

std::vector<std::string> scanNvidiaNIM(pu::http::HttpClient& http) {
  const char* key = std::getenv("NVIDIA_API_KEY");
  std::vector<std::string> headers;
  if (key && *key) {
    headers.push_back(std::string("Authorization: Bearer ") + key);
  }
  return ParseOpenAIModels(http.Get(kNvidiaModelsUrl, headers));
}

std::vector<std::string> scanOllama() {
  pu::http::CurlHttpClient http;
  return scanOllama(http);
}

std::vector<std::string> scanOllama(pu::http::HttpClient& http) {
  return ParseOllamaModels(http.Get(kOllamaTagsUrl));
}

std::vector<std::string> scanOpenAICompatible(const std::string& host,
                                              const std::string& api_key) {
  pu::http::CurlHttpClient http;
  return scanOpenAICompatible(host, api_key, http);
}

std::vector<std::string> scanOpenAICompatible(const std::string& host,
                                              const std::string& api_key,
                                              pu::http::HttpClient& http) {
  std::vector<std::string> headers;
  if (!api_key.empty()) {
    headers.push_back("Authorization: Bearer " + api_key);
  }
  std::string url = host;
  if (!url.empty() && url.back() == '/') url.pop_back();
  url += "/models";
  return ParseOpenAIModels(http.Get(url, headers));
}

}  // namespace pu::config_tools
