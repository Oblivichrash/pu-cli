// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/runtime_spec.hpp"

namespace pu {

nlohmann::json BackendConfig::Serialize() const {
  nlohmann::json j;
  j["type"] = type;
  j["host"] = host;
  j["model"] = model;
  j["api_key"] = api_key;
  j["temperature"] = temperature;
  j["max_tokens"] = max_tokens;
  j["parameters_as_string"] = parameters_as_string;
  return j;
}

BackendConfig BackendConfig::Deserialize(const nlohmann::json& j) {
  BackendConfig cfg;
  if (j.contains("type")) cfg.type = j["type"].get<std::string>();
  if (j.contains("host")) cfg.host = j["host"].get<std::string>();
  if (j.contains("model")) cfg.model = j["model"].get<std::string>();
  if (j.contains("api_key")) cfg.api_key = j["api_key"].get<std::string>();
  if (j.contains("temperature")) cfg.temperature = j["temperature"].get<float>();
  if (j.contains("max_tokens")) cfg.max_tokens = j["max_tokens"].get<int>();
  if (j.contains("parameters_as_string")) cfg.parameters_as_string = j["parameters_as_string"].get<bool>();
  return cfg;
}

nlohmann::json RuntimeSpec::Serialize() const {
  nlohmann::json j;
  j["backend"] = backend.Serialize();
  j["agent_name"] = agent_name;
  j["max_delegation_depth"] = max_delegation_depth;
  j["overrides"] = overrides;
  return j;
}

RuntimeSpec RuntimeSpec::Deserialize(const nlohmann::json& j) {
  RuntimeSpec spec;
  if (j.contains("backend")) spec.backend = BackendConfig::Deserialize(j["backend"]);
  if (j.contains("agent_name")) spec.agent_name = j["agent_name"].get<std::string>();
  if (j.contains("max_delegation_depth")) spec.max_delegation_depth = j["max_delegation_depth"].get<int>();
  if (j.contains("overrides")) spec.overrides = j["overrides"].get<std::map<std::string, nlohmann::json>>();
  return spec;
}

} // namespace pu
