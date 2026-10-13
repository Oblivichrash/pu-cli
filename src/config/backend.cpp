// SPDX-License-Identifier: GPL-3.0-only
#include "pu/config/backend.hpp"

#include "pu/core/base.hpp"
#include "pu/llm/codebuddy.hpp"
#include "pu/llm/ollama_provider.hpp"
#include "pu/llm/openai_provider.hpp"

#include <memory>
#include <utility>

namespace pu::config {

std::unique_ptr<pu::LLMProvider> CreateBackend(const BackendConfig& cfg,
                                               std::shared_ptr<pu::http::HttpClient> http) {
  if (cfg.host.empty()) {
    throw pu::Error("Missing host for backend type: " + std::string(BackendTypeName(cfg.type)));
  }

  const auto http_config = [&cfg]() {
    OpenAIProvider::Config http_cfg;
    http_cfg.model = cfg.model;
    http_cfg.temperature = cfg.temperature;
    http_cfg.host = cfg.host;
    http_cfg.api_key = cfg.api_key.value_or("");
    http_cfg.max_tokens = cfg.max_tokens;
    http_cfg.thinking = cfg.thinking;
    return http_cfg;
  };

  switch (cfg.type) {
    case BackendType::kOllama: {
      OllamaProvider::Config ollama_cfg;
      ollama_cfg.model = cfg.model;
      ollama_cfg.temperature = cfg.temperature;
      ollama_cfg.host = cfg.host;
      ollama_cfg.api_key = cfg.api_key.value_or("");
      ollama_cfg.thinking = cfg.thinking;
      return std::make_unique<OllamaProvider>(std::move(ollama_cfg), std::move(http));
    }
    case BackendType::kOpenAI:
      return std::make_unique<OpenAIProvider>(http_config(), std::move(http));
    case BackendType::kCodeBuddy: {
      OpenAIProvider::Config codebuddy_cfg = http_config();
      const std::string conversation_id = llm::CodeBuddyConversationId();
      codebuddy_cfg.extra_headers = [conversation_id] {
        return llm::CodeBuddyHeaders(conversation_id);
      };
      return std::make_unique<OpenAIProvider>(codebuddy_cfg, std::move(http));
    }
    default:
      throw pu::Error("Unknown backend type");
  }
}

std::vector<std::string> ListModels(const BackendConfig& cfg, pu::http::HttpClient& http) {
  if (cfg.host.empty()) {
    throw pu::Error("Missing host for backend type: " + std::string(BackendTypeName(cfg.type)));
  }

  // A host must not end in a slash; the path is appended verbatim, as with the chat
  // endpoints.
  std::string url = cfg.host;
  std::vector<std::string> headers;
  if (cfg.type == BackendType::kOllama) {
    url += "/api/tags";
  } else {
    url += "/models";
  }
  if (cfg.api_key && !cfg.api_key->empty()) {
    headers.push_back("Authorization: Bearer " + *cfg.api_key);
  }

  const http::Response response = http.Get(url, headers);
  if (response.body.empty()) return {};

  boost::json::value parsed;
  try {
    parsed = boost::json::parse(response.body);
  } catch (const boost::system::system_error&) {
    throw pu::Error("Model listing from " + url + " does not parse as JSON");
  }
  if (!parsed.is_object()) return {};

  const auto& obj = parsed.as_object();
  const boost::json::value* container = nullptr;
  if (cfg.type == BackendType::kOllama) {
    const auto it = obj.find("models");
    container = it != obj.end() ? &it->value() : nullptr;
  } else {
    const auto it = obj.find("data");
    container = it != obj.end() ? &it->value() : nullptr;
  }
  if (container == nullptr || !container->is_array()) return {};

  std::vector<std::string> models;
  for (const auto& entry : container->as_array()) {
    if (!entry.is_object()) continue;
    const auto& entry_obj = entry.as_object();
    const char* field = cfg.type == BackendType::kOllama ? "name" : "id";
    const auto it = entry_obj.find(field);
    if (it != entry_obj.end() && it->value().is_string()) {
      models.push_back(boost::json::value_to<std::string>(it->value()));
    }
  }
  return models;
}

}  // namespace pu::config
