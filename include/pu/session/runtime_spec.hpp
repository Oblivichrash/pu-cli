// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <map>
#include <nlohmann/json.hpp>

#include "pu/agent_config.hpp"

namespace pu {

// RuntimeSpec uses the unified config::BackendConfig for its backend field.
// The JSON serialization is defined via NLOHMANN_DEFINE_TYPE_INTRUSIVE which
// will invoke the custom to_json/from_json defined in agent_config.hpp.
struct RuntimeSpec {
  config::BackendConfig backend;
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