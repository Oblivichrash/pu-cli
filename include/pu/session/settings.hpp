// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The settings a session may override, and the certificate a value needs to be accepted.
//
// One table, not one field per setting: each entry says its name, how a value is read from JSON,
// how it is written back, and what the agent's own configuration supplies when this session has
// not overridden it. Adding a setting is a row rather than a change in four files.

#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "pu/config/backend.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu::session {

// Where a setting's effective value came from.
inline constexpr const char* kSourceSession = "session";
inline constexpr const char* kSourceAgent = "agent";

// One setting's current answer.
struct SettingView {
  std::string name;
  std::string type;   // "enum", "int", "float"
  boost::json::value value;
  std::string source;  // kSourceSession or kSourceAgent
  // The agent's own value, so a page can say what a reset would go back to.
  boost::json::value agent_value;
};

// The overrides a session carries, by name. A name that is not in the table below is refused
// before it is stored, so a typo is an error rather than a value nothing reads.
using Overrides = boost::json::object;

// Reads a setting's value out of a request body. `null` (or a missing member, which the caller
// turns into the same thing) clears the override. Anything else that does not fit the setting's
// type is refused here, with a message naming the setting.
boost::json::value ParseSetting(const std::string& name, const boost::json::value& raw);

// Every setting this build knows, with the value in force: the session's override where there
// is one, and the agent's own configuration otherwise.
std::vector<SettingView> ViewSettings(const config::Backend& agent_backend,
                                      int agent_tool_budget, const Overrides& overrides);

// What the session override changes about the agent's backend. `thinking` is folded in here
// because the provider reads a Backend rather than a set of overrides.
config::Backend ApplyOverrides(const config::Backend& agent_backend, const Overrides& overrides);

// The tool-call budget in force: the session's override where there is one.
int EffectiveToolBudget(int agent_budget, const Overrides& overrides);

}  // namespace pu::session
