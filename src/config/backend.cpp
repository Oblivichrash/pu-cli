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
  // A backend is reached at a host, and the default for a type chosen without one is
  // answered where that choice is made (/backend). Reaching this without a host is a
  // malformed configuration, and a request to "/chat/completions" is a worse way to
  // find that out than an error that says which type it was.
  if (cfg.host.empty()) {
    throw pu::Error("Missing host for backend type: " + std::string(BackendTypeName(cfg.type)));
  }

  // The two HTTP backends speak the same protocol, so what they have in common is
  // filled once and the differences are the name the gateway is called by.
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
      // The same protocol as OpenAI, at a gateway that has to be told who is
      // calling and expects to be reached as the CodeBuddy client is.
      OpenAIProvider::Config codebuddy_cfg = http_config();
      codebuddy_cfg.extra_headers = [] { return llm::CodeBuddyHeaders(); };
      return std::make_unique<OpenAIProvider>(codebuddy_cfg, std::move(http));
    }
    default:
      throw pu::Error("Unknown backend type");
  }
}

}  // namespace pu::config
