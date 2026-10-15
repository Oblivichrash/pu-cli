// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/model_verifier.hpp"

#include <boost/json.hpp>

#include "pu/context/message.hpp"

namespace pu::llm {

namespace {

constexpr int kProbeMaxTokens = 1;

ChatRequest ProbeRequest(std::span<const ChatMessage> history) {
  ChatRequest request(history, {});
  request.on_content = [](std::string_view) {};
  return request;
}

// A level is what a model whose catalogue entry says `onlyReasoning` is answered under:
// measured, a turn that carries none is refused `400 11150`, and any level is accepted.
bool SendsNoLevel(ThinkingLevel level) {
  return level == ThinkingLevel::kNone || level == ThinkingLevel::kServerDefault;
}

std::string MissingLevelReason(const ModelEntry& entry) {
  std::string reason =
      "this model is answered only with a reasoning level and this backend sends none; "
      "set thinking to low, medium or high";
  if (!entry.caps.default_effort.empty()) {
    reason += " (the catalogue's own default is " + entry.caps.default_effort + ")";
  }
  return reason;
}

}  // namespace

std::vector<ModelCheck> VerifyModels(const config::BackendConfig& cfg,
                                     const std::vector<ModelEntry>& models,
                                     std::shared_ptr<http::HttpClient> http) {
  std::vector<ModelCheck> checks;
  checks.reserve(models.size());

  ChatMessage ping;
  ping.role = context::kUserRole;
  ping.content = "ping";
  const std::span<const ChatMessage> history(&ping, 1);

  for (const ModelEntry& entry : models) {
    ModelCheck check;
    check.model = entry.id;

    if (entry.caps.only_reasoning && SendsNoLevel(cfg.thinking)) {
      // Not asked: the host would answer "no effort", which reports the level rather than
      // the model, and the catalogue has already said which level is missing.
      check.detail = MissingLevelReason(entry);
      checks.push_back(std::move(check));
      continue;
    }

    config::BackendConfig probe = cfg;
    probe.model = entry.id;
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
