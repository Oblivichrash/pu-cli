// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "pu/core/http_client.hpp"
#include "pu/core/json.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu::config {

enum class BackendType { kOllama, kOpenAI, kCodeBuddy };

inline constexpr BackendType kBackendTypes[] = {BackendType::kOllama, BackendType::kOpenAI,
                                                BackendType::kCodeBuddy};

// A type keeps no default host and no name of its own: both come from the gate that
// answers for it, so a type whose gate differs cannot disagree with itself.
std::string DefaultHostFor(BackendType type);
const char* BackendTypeName(BackendType type);
std::string BackendTypeNames();
std::optional<BackendType> ParseBackendType(std::string_view name);

struct BackendConfig {
  BackendType type = BackendType::kOllama;
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


inline void tag_invoke(boost::json::value_from_tag, boost::json::value& j,
                       const BackendConfig& cfg) {
  j = {
      {"gateway", BackendTypeName(cfg.type)},
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
  BackendConfig cfg;
  const auto gateway_name = json::ValueOrDefault<std::string>(j, "gateway", "ollama");
  const auto parsed = ParseBackendType(gateway_name);
  if (!parsed) throw Error("Unknown gateway: " + gateway_name);
  cfg.type = *parsed;
  cfg.host = json::ValueOrDefault<std::string>(j, "host", "");
  cfg.model = json::ValueOrDefault<std::string>(j, "model", "");
  if (json::HasKey(j, "api_key") && j.at("api_key").is_string()) {
    auto key = boost::json::value_to<std::string>(j.at("api_key"));
    cfg.api_key = key.empty() ? std::optional<std::string>{} : key;
  }
  cfg.temperature = json::ValueOrDefault<float>(j, "temperature", 0.7f);
  cfg.max_tokens = json::OptionalOrDefault<int>(j, "max_tokens");
  cfg.system_prompt = std::nullopt;
  cfg.thinking = ReadThinkingLevel(j);
  return cfg;
}

std::unique_ptr<pu::LLMProvider> CreateBackend(const BackendConfig& cfg,
                                               std::shared_ptr<pu::http::HttpClient> http);

// Reads the model listing the backend's host reports and returns the ids it names. The
// gate answers where that listing lives. A listing that names no model is an empty
// vector, not an error. Throws HttpError on a failed read.
std::vector<std::string> ListModels(const BackendConfig& cfg, pu::http::HttpClient& http);

}  // namespace pu::config
