// SPDX-License-Identifier: GPL-3.0-only
#include "pu/config/gateway.hpp"

#include "pu/config/backend.hpp"
#include "pu/core/base.hpp"

#include "pu/llm/model_listing.hpp"
#include "pu/llm/ollama_provider.hpp"
#include "pu/llm/openai_provider.hpp"

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pu::config {

OpenAIProvider::Config OpenAIConfig(const Backend& cfg);

namespace {

template <typename Config>
void FillProtocolConfig(Config& protocol, const Backend& cfg) {
  protocol.host = cfg.host;
  protocol.model = cfg.model;
  protocol.temperature = cfg.temperature;
  protocol.api_key = cfg.api_key.value_or("");
  protocol.thinking = cfg.thinking;
}

std::unique_ptr<pu::LLMProvider> MakeOllamaProvider(const Backend& cfg,
                                                    std::shared_ptr<pu::http::HttpClient> http) {
  OllamaProvider::Config protocol;
  FillProtocolConfig(protocol, cfg);
  return std::make_unique<OllamaProvider>(std::move(protocol), std::move(http));
}

std::unique_ptr<pu::ModelLister> MakeOllamaListing() {
  return std::make_unique<llm::OllamaModelLister>();
}

std::unique_ptr<pu::LLMProvider> MakeOpenAIProvider(const Backend& cfg,
                                                    std::shared_ptr<pu::http::HttpClient> http) {
  return std::make_unique<OpenAIProvider>(OpenAIConfig(cfg), std::move(http));
}

std::unique_ptr<pu::ModelLister> MakeOpenAIListing() {
  return std::make_unique<llm::OpenAIModelLister>();
}

std::string CodeBuddyConversationId() { return uuid::Generate(); }

std::vector<std::string> CodeBuddyHeaders(const std::string& conversation_id) {
  constexpr const char* kClientVersion = "1.0.7";
  const std::string version = kClientVersion;
  const auto hex_id = [] {
    std::string hex;
    hex.reserve(32);
    for (const char c : uuid::Generate()) {
      if (c != '-') hex += c;
    }
    return hex;
  };
  return {
      "X-Requested-With: XMLHttpRequest",
      "X-Domain: www.codebuddy.ai",
      "X-Product: SaaS",
      "X-Agent-Intent: craft",
      "X-IDE-Type: CLI",
      "X-IDE-Name: CLI",
      "X-IDE-Version: " + version,
      "User-Agent: CLI/" + version + " CodeBuddy/" + version,
      "X-Conversation-ID: " + conversation_id,
      "X-Conversation-Request-ID: " + hex_id(),
      "X-Conversation-Message-ID: " + hex_id(),
      "X-Request-ID: " + hex_id(),
      "X-User-Id: " + uuid::Generate(),
      "x-stainless-lang: js",
      "x-stainless-package-version: 5.10.1",
      "x-stainless-runtime: node",
  };
}

std::unique_ptr<pu::LLMProvider> MakeCodeBuddyProvider(const Backend& cfg,
                                                       std::shared_ptr<pu::http::HttpClient> http) {
  OpenAIProvider::Config protocol = OpenAIConfig(cfg);
  const std::string conversation_id = CodeBuddyConversationId();
  protocol.extra_headers = [conversation_id] { return CodeBuddyHeaders(conversation_id); };
  return std::make_unique<OpenAIProvider>(std::move(protocol), std::move(http));
}

std::unique_ptr<pu::ModelLister> MakeCodeBuddyListing() {
  return std::make_unique<llm::CodeBuddyModelLister>(CodeBuddyHeaders(CodeBuddyConversationId()));
}

std::vector<std::string> CodeBuddyListingHeaders() {
  return CodeBuddyHeaders(CodeBuddyConversationId());
}

std::vector<std::string> NoListingHeaders() { return {}; }

}  // namespace

OpenAIProvider::Config OpenAIConfig(const Backend& cfg) {
  OpenAIProvider::Config protocol;
  FillProtocolConfig(protocol, cfg);
  protocol.max_tokens = cfg.max_tokens;
  return protocol;
}

namespace {

// A product that speaks the OpenAI wire format and needs nothing of its own: the three
// factories are the protocol's, and the entry differs from its siblings in the id, the name,
// the host and the key variable only.
Gateway OpenAiProduct(GatewayId id, std::string_view name, std::string_view host,
                      std::string_view key_env) {
  return Gateway{
      .id = id,
      .name = name,
      .default_host = host,
      .api_key_env = key_env,
      .protocol = "openai",
      .endpoint_path = "/chat/completions",
      .make_provider = &MakeOpenAIProvider,
      .make_listing = &MakeOpenAIListing,
      .listing_headers = &NoListingHeaders,
  };
}

// The table, one row per product. A row that needs its own factories or headers is written
// out in full below rather than forced through the helper above.
const std::array<Gateway, 6> kGateways = {
    Gateway{
        .id = GatewayId::kOllama,
        .name = "ollama",
        .default_host = "http://localhost:11434",
        .api_key_env = "",
        .protocol = "ollama",
        .endpoint_path = "/api/chat",
        .make_provider = &MakeOllamaProvider,
        .make_listing = &MakeOllamaListing,
        .listing_headers = &NoListingHeaders,
    },
    // The cloud service speaks both wires: `/api/tags` carries more about each model, and
    // `/v1/models` is the OpenAI route. This row is the OpenAI one, so a caller picks the
    // other by naming `ollama-cloud-native` in an agents.json entry.
    OpenAiProduct(GatewayId::kOllamaCloud, "ollama-cloud", "https://ollama.com/v1",
                  "OLLAMA_API_KEY"),
    OpenAiProduct(GatewayId::kOpenAI, "openai", "https://api.openai.com/v1", "OPENAI_API_KEY"),
    OpenAiProduct(GatewayId::kDeepSeek, "deepseek", "https://api.deepseek.com/v1",
                  "DEEPSEEK_API_KEY"),
    OpenAiProduct(GatewayId::kNim, "nim", "https://integrate.api.nvidia.com/v1",
                  "NVIDIA_API_KEY"),
    Gateway{
        .id = GatewayId::kCodeBuddy,
        .name = "codebuddy",
        .default_host = "https://copilot.tencent.com/v2",
        .api_key_env = "CODEBUDDY_API_KEY",
        .protocol = "openai",
        .endpoint_path = "/chat/completions",
        .make_provider = &MakeCodeBuddyProvider,
        .make_listing = &MakeCodeBuddyListing,
        .listing_headers = &CodeBuddyListingHeaders,
    },
};

}  // namespace

namespace {

std::span<const Gateway> Table() { return kGateways; }

}  // namespace

std::span<const Gateway> Gateways() { return Table(); }

const Gateway* FindGateway(std::string_view name) {
  for (const Gateway& gateway : Table()) {
    if (gateway.name == name) return &gateway;
  }
  return nullptr;
}

const Gateway& GatewayForId(GatewayId id) {
  for (const Gateway& gateway : Table()) {
    if (gateway.id == id) return gateway;
  }
  throw GenericError("No gateway answers for " + std::to_string(static_cast<int>(id)));
}

}  // namespace pu::config
