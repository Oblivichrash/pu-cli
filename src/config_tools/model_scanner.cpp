// SPDX-License-Identifier: GPL-3.0-only
#include "model_scanner.hpp"

#include <cstdlib>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "infra/curl_http_client.hpp"

namespace pu::config_tools {

namespace {

// Extracts ids from the provider's model listing. Ollama always uses
// models[].name; other providers follow OpenAI's data[].id shape, where
// list_key encodes the path ("data[].id" -> j["data"][i]["id"]).
std::vector<std::string> ParseModelList(const std::string& body,
                                        const std::string& list_key,
                                        bool is_ollama) {
  std::vector<std::string> ids;
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(body);
  } catch (const std::exception&) {
    return ids;
  }
  std::string array_key = "data";
  std::string field = "id";
  if (is_ollama) {
    array_key = "models";
    field = "name";
  } else if (list_key.size() >= 4) {
    const auto sep = list_key.find("[].");
    if (sep != std::string::npos) {
      array_key = list_key.substr(0, sep);
      field = list_key.substr(sep + 3);
    }
  }
  if (!j.contains(array_key) || !j[array_key].is_array()) return ids;
  for (const auto& item : j[array_key]) {
    if (item.contains(field) && item[field].is_string()) {
      ids.push_back(item[field].get<std::string>());
    }
  }
  return ids;
}

}  // namespace

std::vector<std::string> scanProvider(const ProviderConfig& config) {
  pu::http::CurlHttpClient http;
  return scanProvider(config, http);
}

std::vector<std::string> scanProvider(const ProviderConfig& config,
                                      pu::http::HttpClient& http) {
  std::vector<std::string> headers;
  // Env vars are read at scan time so a key added later is picked up without
  // restarting the wizard or CLI.
  if (!config.auth.env_var.empty()) {
    if (const char* key = std::getenv(config.auth.env_var.c_str());
        key && *key) {
      headers.push_back(std::string("Authorization: Bearer ") + key);
    }
  }
  const std::string url = config.base_url + config.model_endpoint;
  return ParseModelList(http.Get(url, headers), config.model_list_key,
                        config.type == "ollama");
}

}  // namespace pu::config_tools
