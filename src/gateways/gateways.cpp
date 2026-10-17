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

const Gateway& OllamaGateway() {
  static const Gateway gateway{
      .id = GatewayId::kOllama,
      .name = "ollama",
      .default_host = "http://localhost:11434",
      .endpoint_path = "/api/chat",
      .make_provider = &MakeOllamaProvider,
      .make_listing = &MakeOllamaListing,
      .listing_headers = &NoListingHeaders,
  };
  return gateway;
}

const Gateway& OpenAIGateway() {
  static const Gateway gateway{
      .id = GatewayId::kOpenAI,
      .name = "openai",
      .default_host = "https://api.openai.com/v1",
      .endpoint_path = "/chat/completions",
      .make_provider = &MakeOpenAIProvider,
      .make_listing = &MakeOpenAIListing,
      .listing_headers = &NoListingHeaders,
  };
  return gateway;
}

const Gateway& CodeBuddyGateway() {
  static const Gateway gateway{
      .id = GatewayId::kCodeBuddy,
      .name = "codebuddy",
      .default_host = "https://copilot.tencent.com/v2",
      .endpoint_path = "/chat/completions",
      .make_provider = &MakeCodeBuddyProvider,
      .make_listing = &MakeCodeBuddyListing,
      .listing_headers = &CodeBuddyListingHeaders,
  };
  return gateway;
}

// The registry of gateways.

namespace {

const std::array<const Gateway*, 3> kGateways = {
    &OllamaGateway(),
    &OpenAIGateway(),
    &CodeBuddyGateway(),
};

}  // namespace

std::span<const Gateway* const> Gateways() { return kGateways; }

const Gateway* FindGateway(std::string_view name) {
  for (const Gateway* gateway : kGateways) {
    if (gateway->name == name) return gateway;
  }
  return nullptr;
}

const Gateway& GatewayForId(GatewayId id) {
  for (const Gateway* gateway : kGateways) {
    if (gateway->id == id) return *gateway;
  }
  throw GenericError("No gateway answers for " + std::to_string(static_cast<int>(id)));
}

}  // namespace pu::config
