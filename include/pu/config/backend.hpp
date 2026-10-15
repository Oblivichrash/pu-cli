// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "pu/config/gateway.hpp"
#include "pu/core/http_client.hpp"
#include "pu/core/json.hpp"
#include "pu/llm/model_listing.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu::config {

struct BackendConfig {
  GatewayId gateway = GatewayId::kOllama;
  std::string host;
  std::string model;
  std::optional<std::string> api_key;
  float temperature = 0.7f;
  std::optional<std::string> system_prompt;
  // Absent means "send no cap", which is not the same as any number. A provider that
  // caps on its own would otherwise be second-guessed by a default nobody chose.
  std::optional<int> max_tokens;
  ThinkingLevel thinking = ThinkingLevel::kServerDefault;
};

// The gate a backend names, from an agents.json entry or a stored one. Required rather
// than defaulted: a file that never named a gate is one whose author meant another one,
// and the wrong gate's host fails a long way from the line that is wrong.
inline GatewayId ReadGateway(const boost::json::value& j) {
  const std::string name = json::ValueOrDefault<std::string>(j, "gateway", "");
  if (name.empty()) throw Error("Missing gateway in backend: name one of " + GatewayNames());
  const std::optional<GatewayId> parsed = ParseGatewayId(name);
  if (!parsed) throw Error("Unknown gateway: " + name);
  return *parsed;
}


// The fields a stored backend shares with an agents.json entry, read the one way both go
// through so their defaults cannot drift apart.
inline void ReadBackendFields(BackendConfig& cfg, const boost::json::value& j) {
  cfg.gateway = ReadGateway(j);
  cfg.host = json::ValueOrDefault<std::string>(j, "host", "");
  cfg.model = json::ValueOrDefault<std::string>(j, "model", "");
  if (json::HasKey(j, "api_key") && j.at("api_key").is_string()) {
    std::string key = boost::json::value_to<std::string>(j.at("api_key"));
    cfg.api_key = key.empty() ? std::optional<std::string>{} : std::move(key);
  }
  cfg.temperature = json::ValueOrDefault<float>(j, "temperature", 0.7f);
  cfg.max_tokens = json::OptionalOrDefault<int>(j, "max_tokens");
  cfg.thinking = ReadThinkingLevel(j);
}

inline BackendConfig ReadBackend(const boost::json::value& j) {
  BackendConfig cfg;
  ReadBackendFields(cfg, j);
  return cfg;
}


inline void tag_invoke(boost::json::value_from_tag, boost::json::value& j,
                       const BackendConfig& cfg) {
  j = {
      {"gateway", GatewayName(cfg.gateway)},
      {"host", cfg.host},
      {"model", cfg.model},
      {"api_key", cfg.api_key.value_or("")},
      {"temperature", cfg.temperature},
      {"thinking", ThinkingLevelName(cfg.thinking)},
  };
  if (cfg.max_tokens) j.as_object()["max_tokens"] = *cfg.max_tokens;
}

inline BackendConfig tag_invoke(boost::json::value_to_tag<BackendConfig>,
                                const boost::json::value& j) {
  return ReadBackend(j);
}

std::unique_ptr<pu::LLMProvider> CreateBackend(const BackendConfig& cfg,
                                               std::shared_ptr<pu::http::HttpClient> http);

// Reads the model listing the backend's host reports, with what the host says about each
// model. The gate answers where that listing lives. A listing that names no model is an
// empty vector, not an error. Throws HttpError on a failed read.
std::vector<llm::ModelEntry> ListModels(const BackendConfig& cfg, pu::http::HttpClient& http);

}  // namespace pu::config
