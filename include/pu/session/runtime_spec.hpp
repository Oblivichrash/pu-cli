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

  nlohmann::json Serialize() const;
  static BackendConfig Deserialize(const nlohmann::json& j);
};

struct RuntimeSpec {
  BackendConfig backend;
  std::string agent_name;
  int max_delegation_depth = 5;
  std::map<std::string, nlohmann::json> overrides;

  nlohmann::json Serialize() const;
  static RuntimeSpec Deserialize(const nlohmann::json& j);
};

} // namespace pu
