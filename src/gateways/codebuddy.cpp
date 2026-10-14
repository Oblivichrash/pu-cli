// SPDX-License-Identifier: GPL-3.0-only
#include "gateways.hpp"

#include "pu/config/backend.hpp"

#include "pu/llm/codebuddy.hpp"
#include "pu/llm/model_listing.hpp"
#include "pu/llm/openai_provider.hpp"

namespace pu::config {

namespace {

std::unique_ptr<pu::LLMProvider> MakeCodeBuddyProvider(const BackendConfig& cfg,
                                                       std::shared_ptr<pu::http::HttpClient> http) {
  OpenAIProvider::Config protocol;
  protocol.model = cfg.model;
  protocol.temperature = cfg.temperature;
  protocol.host = cfg.host;
  protocol.api_key = cfg.api_key.value_or("");
  protocol.max_tokens = cfg.max_tokens;
  protocol.thinking = cfg.thinking;

  // One conversation id per provider, drawn here rather than at file scope: two providers
  // are two conversations, and the turns of one provider share the name.
  const std::string conversation_id = llm::CodeBuddyConversationId();
  protocol.extra_headers = [conversation_id] { return llm::CodeBuddyHeaders(conversation_id); };
  return std::make_unique<OpenAIProvider>(std::move(protocol), std::move(http));
}

std::unique_ptr<pu::ModelLister> MakeCodeBuddyListing() {
  return std::make_unique<llm::CodeBuddyModelLister>();
}

}  // namespace

const Gateway& CodeBuddyGateway() {
  static const Gateway gateway{
      .id = GatewayId::kCodeBuddy,
      .name = "codebuddy",
      .default_host = "https://copilot.tencent.com/v2",
      .make_provider = &MakeCodeBuddyProvider,
      .make_listing = &MakeCodeBuddyListing,
  };
  return gateway;
}

}  // namespace pu::config
