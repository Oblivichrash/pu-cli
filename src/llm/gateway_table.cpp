// SPDX-License-Identifier: GPL-3.0-only
#include "pu/model/gateway.hpp"

#include "pu/model/backend.hpp"
#include "pu/core/base.hpp"

#include "pu/llm/model_listing.hpp"
#include "pu/llm/ollama_provider.hpp"
#include "pu/llm/openai_provider.hpp"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The table that says which provider a gateway makes. A provider and a lister are `llm`'s
// types, so the row that names them is written here rather than beside the ways to look a
// gateway up: the data about a product stays in `model`, the factories that open it do not.
// Looking a gateway up still works from below `llm`, because the table is installed into
// `model` through `InstallGatewayTable` rather than reached for from underneath.
namespace pu::llm {

namespace {

OpenAIProvider::Config OpenAIConfig(const model::Backend& cfg);

template <typename Config>
void FillProtocolConfig(Config& protocol, const model::Backend& cfg) {
  protocol.host = cfg.host;
  protocol.model = cfg.model;
  protocol.temperature = cfg.temperature;
  protocol.api_key = cfg.api_key.value_or("");
  protocol.thinking = cfg.thinking;
}

std::unique_ptr<pu::LLMProvider> MakeOllamaProvider(const model::Backend& cfg,
                                                    std::shared_ptr<pu::http::HttpClient> http) {
  OllamaProvider::Config protocol;
  FillProtocolConfig(protocol, cfg);
  return std::make_unique<OllamaProvider>(std::move(protocol), std::move(http));
}

std::unique_ptr<pu::ModelLister> MakeOllamaListing() {
  return std::make_unique<OllamaModelLister>();
}

std::unique_ptr<pu::LLMProvider> MakeOpenAIProvider(const model::Backend& cfg,
                                                    std::shared_ptr<pu::http::HttpClient> http) {
  return std::make_unique<OpenAIProvider>(OpenAIConfig(cfg), std::move(http));
}

std::unique_ptr<pu::ModelLister> MakeOpenAIListing() {
  return std::make_unique<OpenAIModelLister>();
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

std::unique_ptr<pu::LLMProvider> MakeCodeBuddyProvider(const model::Backend& cfg,
                                                       std::shared_ptr<pu::http::HttpClient> http) {
  OpenAIProvider::Config protocol = OpenAIConfig(cfg);
  const std::string conversation_id = CodeBuddyConversationId();
  protocol.extra_headers = [conversation_id] { return CodeBuddyHeaders(conversation_id); };
  return std::make_unique<OpenAIProvider>(std::move(protocol), std::move(http));
}

std::unique_ptr<pu::ModelLister> MakeCodeBuddyListing() {
  return std::make_unique<CodeBuddyModelLister>(CodeBuddyHeaders(CodeBuddyConversationId()));
}

std::vector<std::string> CodeBuddyListingHeaders() {
  return CodeBuddyHeaders(CodeBuddyConversationId());
}

std::vector<std::string> NoListingHeaders() { return {}; }

OpenAIProvider::Config OpenAIConfig(const model::Backend& cfg) {
  OpenAIProvider::Config protocol;
  FillProtocolConfig(protocol, cfg);
  protocol.max_tokens = cfg.max_tokens;
  // How this product prices a call is the product's own fact, and the provider is the one
  // reading the answer: it is told here rather than guessing from the wire.
  protocol.billing = model::GatewayForId(cfg.gateway).billing;
  return protocol;
}

// A product that speaks the OpenAI wire format and needs nothing of its own: the three
// factories are the protocol's, and the entry differs from its siblings in the id, the name,
// the host and the key variable only.
model::Gateway OpenAiProduct(model::GatewayId id, std::string_view name, std::string_view host,
                             std::string_view key_env) {
  return model::Gateway{
      .id = id,
      .name = name,
      .default_host = host,
      .api_key_env = key_env,
      .protocol = model::WireProtocol::kOpenAI,
      .endpoint_path = "/chat/completions",
      .make_provider = &MakeOpenAIProvider,
      .make_listing = &MakeOpenAIListing,
      .listing_headers = &NoListingHeaders,
  };
}

// The table, one row per product. A row that needs its own factories or headers is written
// out in full below rather than forced through the helper above.
const std::array<model::Gateway, 6> kGateways = {
    model::Gateway{
        .id = model::GatewayId::kOllama,
        .name = "ollama",
        .default_host = "http://localhost:11434",
        .api_key_env = "",
        .protocol = model::WireProtocol::kOllama,
        .endpoint_path = "/api/chat",
        .make_provider = &MakeOllamaProvider,
        .make_listing = &MakeOllamaListing,
        .listing_headers = &NoListingHeaders,
    },
    // The cloud service answers both wires: `/api/tags` describes each model in more detail and
    // `/v1/models` is the OpenAI route. This row is the OpenAI one. The native route is not a
    // gateway here — nothing in this build claims it, and a name a table does not carry is
    // refused at load.
    OpenAiProduct(model::GatewayId::kOllamaCloud, "ollama-cloud", "https://ollama.com/v1",
                  "OLLAMA_API_KEY"),
    OpenAiProduct(model::GatewayId::kOpenAI, "openai", "https://api.openai.com/v1",
                  "OPENAI_API_KEY"),
    OpenAiProduct(model::GatewayId::kDeepSeek, "deepseek", "https://api.deepseek.com/v1",
                  "DEEPSEEK_API_KEY"),
    OpenAiProduct(model::GatewayId::kNim, "nim", "https://integrate.api.nvidia.com/v1",
                  "NVIDIA_API_KEY"),
    model::Gateway{
        .id = model::GatewayId::kCodeBuddy,
        .name = "codebuddy",
        .default_host = "https://copilot.tencent.com/v2",
        .api_key_env = "CODEBUDDY_API_KEY",
        .protocol = model::WireProtocol::kOpenAI,
        .endpoint_path = "/chat/completions",
        .make_provider = &MakeCodeBuddyProvider,
        .make_listing = &MakeCodeBuddyListing,
        .listing_headers = &CodeBuddyListingHeaders,
        // The one product that says what a call cost. `credit` is the amount of this call,
        // not an account balance, and it is denominated in the product's own credits.
        .billing = {model::BillingKind::kReported, "credit", "credits"},
    },
};

std::span<const model::Gateway> Table() { return kGateways; }

}  // namespace

std::span<const model::Gateway> Rows() { return Table(); }

}  // namespace pu::llm

// The lookup half, defined where a caller below `llm` can reach it. The table is a local of
// this file, so nothing here is a static initialisation: `Installed()` is filled by
// `InstallGatewayTable` at the program's entry point, and before that a lookup falls back to
// the rows this file owns, which are always built.
namespace pu::model {

namespace {

const std::span<const Gateway>*& Installed() {
  static const std::span<const Gateway>* table = nullptr;
  return table;
}

std::span<const Gateway> Fallback() { return llm::Rows(); }

}  // namespace

void InstallGatewayTable() {
  static const std::span<const Gateway> table = Fallback();
  Installed() = &table;
}

std::span<const Gateway> Gateways() {
  const std::span<const Gateway>* table = Installed();
  if (table == nullptr) return Fallback();
  return *table;
}

const Gateway* FindGateway(std::string_view name) {
  for (const Gateway& gateway : Gateways()) {
    if (gateway.name == name) return &gateway;
  }
  return nullptr;
}

const Gateway& GatewayForId(GatewayId id) {
  for (const Gateway& gateway : Gateways()) {
    if (gateway.id == id) return gateway;
  }
  throw GenericError("No gateway answers for " + std::to_string(static_cast<int>(id)));
}

}  // namespace pu::model
