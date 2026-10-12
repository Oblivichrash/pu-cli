// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <boost/json.hpp>

#include "pu/core/http_client.hpp"
#include "pu/core/json.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu::config {

enum class BackendType { kOllama, kOpenAI, kCodeBuddy };

inline constexpr BackendType kBackendTypes[] = {BackendType::kOllama, BackendType::kOpenAI,
                                                BackendType::kCodeBuddy};

// Every backend type carries a thinking level: Ollama as a top-level `think`, the
// OpenAI-compatible ones as `reasoning_effort` / `extra_body`. A backend that cannot
// is a new one, and it will say so rather than being carved out here.
inline std::string DefaultHostFor(BackendType type) {
  switch (type) {
    case BackendType::kOllama:
      return "http://localhost:11434";
    case BackendType::kOpenAI:
      return "https://api.openai.com/v1";
    case BackendType::kCodeBuddy:
      return "https://copilot.tencent.com/v2";
  }
  return "";
}

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

inline const char* BackendTypeName(BackendType type) {
  switch (type) {
    case BackendType::kOllama:
      return "ollama";
    case BackendType::kOpenAI:
      return "openai";
    case BackendType::kCodeBuddy:
      return "codebuddy";
  }
  return "ollama";
}

inline std::string BackendTypeNames() {
  std::string names;
  for (const BackendType type : kBackendTypes) {
    if (!names.empty()) names += ", ";
    names += BackendTypeName(type);
  }
  return names;
}

inline std::optional<BackendType> ParseBackendType(std::string_view name) {
  if (name == "ollama") return BackendType::kOllama;
  if (name == "openai") return BackendType::kOpenAI;
  if (name == "codebuddy") return BackendType::kCodeBuddy;
  return std::nullopt;
}

inline void tag_invoke(boost::json::value_from_tag, boost::json::value& j,
                       const BackendConfig& cfg) {
  j = {
      {"type", BackendTypeName(cfg.type)},
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
  const auto type_str = json::ValueOrDefault<std::string>(j, "type", "ollama");
  cfg.type = ParseBackendType(type_str).value_or(BackendType::kOllama);
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

}  // namespace pu::config
