// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/model_verifier.hpp"

#include <boost/json.hpp>

#include "pu/context/message.hpp"

namespace pu::llm {

namespace {

// One token is enough to prove the model ran: the reply's text is not read, only
// whether a reply came back at all.
constexpr int kProbeMaxTokens = 1;

ChatRequest ProbeRequest(std::span<const ChatMessage> history) {
  ChatRequest request(history, {});
  request.on_content = [](std::string_view) {};
  return request;
}

}  // namespace

std::vector<ModelCheck> VerifyModels(const config::BackendConfig& cfg,
                                     const std::vector<std::string>& models,
                                     std::shared_ptr<http::HttpClient> http) {
  std::vector<ModelCheck> checks;
  checks.reserve(models.size());

  ChatMessage ping;
  ping.role = context::kUserRole;
  ping.content = "ping";
  const std::span<const ChatMessage> history(&ping, 1);

  for (const std::string& model : models) {
    ModelCheck check;
    check.model = model;

    config::BackendConfig probe = cfg;
    probe.model = model;
    probe.max_tokens = kProbeMaxTokens;

    try {
      std::unique_ptr<LLMProvider> provider = config::CreateBackend(probe, http);
      provider->Chat(ProbeRequest(history));
      check.answered = true;
    } catch (const std::exception& e) {
      check.detail = e.what();
    }

    checks.push_back(std::move(check));
  }

  return checks;
}

}  // namespace pu::llm
