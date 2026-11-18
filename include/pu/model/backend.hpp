// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "pu/llm/llm_types.hpp"
#include "pu/model/gateway.hpp"
#include "pu/core/http_client.hpp"
#include "pu/core/json.hpp"
#include "pu/model/listing.hpp"


namespace pu::model {

struct Backend {
  GatewayId gateway = GatewayId::kOllama;
  // Empty until a gateway's own host fills it, which ReadBackendFields does and so does every
  // caller that builds a backend from a name.
  std::string host;
  std::string model;
  std::optional<std::string> api_key;
  float temperature = 0.7f;
  std::optional<std::string> system_prompt;
  // Absent means "send no cap", which is not the same as any number. A provider that
  // caps on its own would otherwise be second-guessed by a default nobody chose.
  std::optional<int> max_tokens;
  ThinkingLevel thinking = ThinkingLevel::kServerDefault;
  // The nucleus sampler. Absent means "send none", which leaves the provider's own default in
  // place rather than pinning 1.0: a product whose default is not 1.0 would otherwise be
  // silently narrowed by a number nobody chose.
  std::optional<float> top_p;
  // Makes two turns with the same prompt repeatable where a provider honours it. Absent for
  // the same reason as `top_p`.
  std::optional<int> seed;
  // Sequences that end the reply early, sent as the wire's `stop`. Empty means none: a
  // provider that defaults to something of its own is not overridden by an empty list.
  std::vector<std::string> stop;
  // A JSON schema the reply must satisfy, sent as the wire's `response_format`. Absent means
  // the reply is prose, which is what every caller that does not name one wants. Present, a
  // gateway that honours it either answers conforming JSON or refuses the request, and the
  // second is the outcome worth having: a malformed tool argument discovered here is a
  // refused turn rather than a parse failure two layers up.
  std::optional<boost::json::value> response_schema;
  // How much context the model holds, when the file says so. Advisory: nothing trims a prompt
  // against it and nothing refuses a turn that outgrows it, because a gateway that truncates
  // silently would turn a wrong number into a bug nobody could see. Named here it outranks
  // whatever the gateway's catalogue reports, which is how a nominal figure is corrected by
  // hand without waiting for the product to change its mind.
  std::optional<int> context_window;
};

// The gateway a backend names, from an agents.json entry or a stored one. Required rather
// than defaulted: a file that never named a gateway is one whose author meant another one,
// and the wrong gateway's host fails a long way from the line that is wrong.
inline GatewayId ReadGateway(const boost::json::value& j) {
  const std::string name = json::ValueOrDefault<std::string>(j, "gateway", "");
  if (name.empty()) throw GenericError("Missing gateway in backend: name one of " + GatewayNames());
  const std::optional<GatewayId> parsed = ParseGatewayId(name);
  if (!parsed) throw GenericError("Unknown gateway: " + name);
  return *parsed;
}


// The fields a stored backend shares with an agents.json entry, read the one way both go
// through so their defaults cannot drift apart.
inline void ReadBackendFields(Backend& cfg, const boost::json::value& j) {
  cfg.gateway = ReadGateway(j);
  cfg.host = json::ValueOrDefault<std::string>(j, "host", "");
  cfg.model = json::ValueOrDefault<std::string>(j, "model", "");
  if (json::HasKey(j, "api_key") && j.at("api_key").is_string()) {
    std::string key = boost::json::value_to<std::string>(j.at("api_key"));
    cfg.api_key = key.empty() ? std::optional<std::string>{} : std::move(key);
  }
  cfg.temperature = json::ValueOrDefault<float>(j, "temperature", 0.7f);
  cfg.max_tokens = json::OptionalOrDefault<int>(j, "max_tokens");
  cfg.top_p = json::OptionalOrDefault<float>(j, "top_p");
  cfg.seed = json::OptionalOrDefault<int>(j, "seed");
  cfg.stop = json::StringArrayOrEmpty(j, "stop");
  if (json::HasKey(j, "response_schema")) cfg.response_schema = j.at("response_schema");
  cfg.context_window = json::OptionalOrDefault<int>(j, "context_window");
  cfg.thinking = ReadThinkingLevel(j);
}

inline Backend ReadBackend(const boost::json::value& j) {
  Backend cfg;
  ReadBackendFields(cfg, j);
  return cfg;
}


inline void tag_invoke(boost::json::value_from_tag, boost::json::value& j,
                       const Backend& cfg) {
  j = {
      {"gateway", GatewayName(cfg.gateway)},
      {"host", cfg.host},
      {"model", cfg.model},
      {"api_key", cfg.api_key.value_or("")},
      {"temperature", cfg.temperature},
      {"thinking", ThinkingLevelName(cfg.thinking)},
  };
  if (cfg.max_tokens) j.as_object()["max_tokens"] = *cfg.max_tokens;
  if (cfg.top_p) j.as_object()["top_p"] = *cfg.top_p;
  if (cfg.seed) j.as_object()["seed"] = *cfg.seed;
  if (!cfg.stop.empty()) j.as_object()["stop"] = boost::json::value_from(cfg.stop);
  if (cfg.response_schema) j.as_object()["response_schema"] = *cfg.response_schema;
  if (cfg.context_window) j.as_object()["context_window"] = *cfg.context_window;
}

inline Backend tag_invoke(boost::json::value_to_tag<Backend>,
                                const boost::json::value& j) {
  return ReadBackend(j);
}

// The gateway entry a backend names, with the one check every caller needs: an empty host means
// the config wrote `"host": ""` rather than that the gateway has none, and a backend that got
// here without its host filled in is a configuration the caller should hear about by name.
inline const Gateway& GatewayForConfig(const Backend& cfg) {
  if (cfg.host.empty()) {
    throw GenericError("Missing host for gateway: " + std::string(GatewayName(cfg.gateway)));
  }
  return GatewayForId(cfg.gateway);
}

// A backend built from a gateway name alone: its host and its key come from the table and
// the environment the table names, so one word on the command line reaches a product. A
// gateway whose key variable is unset is reached without a key rather than with a literal.
Backend BackendForGateway(std::string_view gateway);

}  // namespace pu::model
