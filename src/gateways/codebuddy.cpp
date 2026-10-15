// SPDX-License-Identifier: GPL-3.0-only
#include "gateways.hpp"

#include "pu/config/backend.hpp"
#include "pu/core/base.hpp"

#include "pu/llm/model_listing.hpp"
#include "pu/llm/openai_provider.hpp"

#include <string>
#include <vector>

namespace pu::config {

namespace {

// The gateway accepts a generated id where the real client sends an account user id.
std::string CodeBuddyUserId() { return uuid::Generate(); }

// The header a conversation's turns are all sent under. The caller supplies it so a
// session's turns carry one name; the per-request ids below are what vary between calls.
std::string CodeBuddyConversationId() { return uuid::Generate(); }

// Headers the installed client sends, imitated rather than derived from what the server
// checks. `conversation_id` is supplied by the caller so that every turn of a session
// carries the same conversation name. Both a turn and a catalogue read are sent under
// them, which is why the gate builds both and the lister is handed the result.
std::vector<std::string> CodeBuddyHeaders(const std::string& conversation_id) {
  constexpr const char* kClientVersion = "1.0.7";
  const std::string version = kClientVersion;
  // A uuid with its dashes removed: the client's shape for the ids that name an event
  // rather than a thing.
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
      "X-User-Id: " + CodeBuddyUserId(),
      "x-stainless-lang: js",
      "x-stainless-package-version: 5.10.1",
      "x-stainless-runtime: node",
  };
}

std::unique_ptr<pu::LLMProvider> MakeCodeBuddyProvider(const BackendConfig& cfg,
                                                       std::shared_ptr<pu::http::HttpClient> http) {
  OpenAIProvider::Config protocol = OpenAIConfig(cfg);

  // One conversation id per provider, drawn here rather than at file scope: two providers
  // are two conversations, and the turns of one provider share the name.
  const std::string conversation_id = CodeBuddyConversationId();
  protocol.extra_headers = [conversation_id] { return CodeBuddyHeaders(conversation_id); };
  return std::make_unique<OpenAIProvider>(std::move(protocol), std::move(http));
}

std::unique_ptr<pu::ModelLister> MakeCodeBuddyListing() {
  return std::make_unique<llm::CodeBuddyModelLister>(CodeBuddyHeaders(CodeBuddyConversationId()));
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
