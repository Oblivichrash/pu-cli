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
                                               std::unique_ptr<pu::http::HttpClient> http) {
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
      return std::make_unique<OllamaProvider>(std::move(ollama_cfg), std::move(http));
    }
    case BackendType::kOpenAI:
      return std::make_unique<OpenAIProvider>(http_config(), std::move(http));
    case BackendType::kCodeBuddy: {
      OpenAIProvider::Config codebuddy_cfg = http_config();
      codebuddy_cfg.extra_headers = [] { return llm::CodeBuddyHeaders(); };
      return std::make_unique<OpenAIProvider>(codebuddy_cfg, std::move(http));
    }
    default:
      throw pu::Error("Unknown backend type");
  }
}

}  // namespace pu::config
