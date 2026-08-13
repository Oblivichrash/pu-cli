// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/provider_factory.hpp"
#include "pu/error.hpp"
#include "pu/llm/ollama_provider.hpp"
#include "pu/llm/openai_provider.hpp"

namespace pu::config {

std::unique_ptr<pu::LLMProvider> CreateBackend(
    const BackendConfig& cfg, std::unique_ptr<pu::http::HttpClient> http) {
  switch (cfg.type) {
    case BackendType::kOllama: {
      OllamaProvider::Config ollama_cfg;
      ollama_cfg.model = cfg.model;
      ollama_cfg.temperature = cfg.temperature;
      ollama_cfg.system_prompt = cfg.system_prompt;
      ollama_cfg.host = cfg.host;
      ollama_cfg.api_key = cfg.api_key.value_or("");
      ollama_cfg.max_tokens = cfg.max_tokens;
      return std::make_unique<OllamaProvider>(std::move(ollama_cfg), std::move(http));
    }
    case BackendType::kOpenAI: {
      OpenAIProvider::Config openai_cfg;
      openai_cfg.model = cfg.model;
      openai_cfg.temperature = cfg.temperature;
      openai_cfg.system_prompt = cfg.system_prompt;
      openai_cfg.host = cfg.host;
      openai_cfg.api_key = cfg.api_key.value_or("");
        openai_cfg.max_tokens = cfg.max_tokens;
      openai_cfg.enable_thinking = cfg.enable_thinking;
      openai_cfg.reasoning_effort = cfg.reasoning_effort;
      return std::make_unique<OpenAIProvider>(openai_cfg, std::move(http));
    }
    default:
      throw pu::RuntimeError("Unknown backend type");
  }
}

}  // namespace pu::config
