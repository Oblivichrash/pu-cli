// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <map>
#include <nlohmann/json.hpp>

namespace pu {

struct BackendConfig {
  std::string type;   // "ollama" | "openai"
  std::string host;
  std::string model;
  std::string api_key;
  float temperature = 0.7f;
  int max_tokens = 2048;
  bool parameters_as_string = false;

  // Thin wrappers for backward compatibility with existing callers.
  nlohmann::json Serialize() const { return nlohmann::json(*this); }
  static BackendConfig Deserialize(const nlohmann::json& j) { return j.get<BackendConfig>(); }

  NLOHMANN_DEFINE_TYPE_INTRUSIVE(BackendConfig, type, host, model, api_key,
                                 temperature, max_tokens, parameters_as_string)
};

struct RuntimeSpec {
  BackendConfig backend;
  std::string agent_name;
  int max_delegation_depth = 5;
  std::map<std::string, nlohmann::json> overrides;

  // Thin wrappers for backward compatibility with existing callers.
  nlohmann::json Serialize() const { return nlohmann::json(*this); }
  static RuntimeSpec Deserialize(const nlohmann::json& j) { return j.get<RuntimeSpec>(); }

  NLOHMANN_DEFINE_TYPE_INTRUSIVE(RuntimeSpec, backend, agent_name,
                                max_delegation_depth, overrides)
};

} // namespace pu
