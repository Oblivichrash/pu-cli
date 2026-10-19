// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/settings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <string_view>

#include "pu/core/base.hpp"
#include "pu/core/json.hpp"

namespace pu::session {

namespace {

namespace json = pu::json;

using Reader = std::function<boost::json::value(const boost::json::value&)>;
using Writer = std::function<boost::json::value(const config::Backend&, int)>;

// One setting: its name, the type a page draws it as, how a value is read, and what the agent's
// configuration supplies when the session has not overridden it.
struct Setting {
  const char* name;
  const char* type;
  Reader read;
  Writer agent_value;
};

std::string ThinkingName(ThinkingLevel level) {
  return level == ThinkingLevel::kServerDefault ? std::string("auto") : ThinkingLevelName(level);
}

boost::json::value ReadThinking(const boost::json::value& raw) {
  const std::string name = raw.is_string() ? boost::json::value_to<std::string>(raw) : "";
  if (name == "auto" || name == "default") return boost::json::value{};  // clears the override
  const ThinkingLevel level = ParseThinkingLevel(name);
  if (name != "none" && level == ThinkingLevel::kServerDefault) {
    throw RequestRefused("Unknown thinking level: " + name + " (none, low, medium, high, auto)");
  }
  return boost::json::string(name);
}

boost::json::value ReadTemperature(const boost::json::value& raw) {
  double value = 0;
  if (raw.is_double()) {
    value = raw.as_double();
  } else if (raw.is_int64()) {
    value = static_cast<double>(raw.as_int64());
  } else {
    throw RequestRefused("temperature must be a number");
  }
  if (value < 0.0 || value > 2.0) throw RequestRefused("temperature must be between 0 and 2");
  return value;
}

boost::json::value ReadMaxTokens(const boost::json::value& raw) {
  if (!raw.is_int64()) throw RequestRefused("max_tokens must be a whole number");
  const auto value = raw.as_int64();
  if (value < 1 || value > 1000000) {
    throw RequestRefused("max_tokens must be between 1 and 1000000");
  }
  return value;
}

boost::json::value ReadToolBudget(const boost::json::value& raw) {
  if (!raw.is_int64()) throw RequestRefused("tool_budget must be a whole number");
  // Zero is not "unlimited": it would stop the turn at its first tool call, which is a value
  // nobody asks for on purpose. A caller that wants no limit says `null`, which clears the
  // override and leaves the agent's number.
  const auto value = raw.as_int64();
  if (value < 1 || value > 100000) {
    throw RequestRefused("tool_budget must be between 1 and 100000");
  }
  return value;
}

// The table. A row is the whole of adding a setting: the request parser, the view and the
// effective value all read it.
const std::array<Setting, 4>& Table() {
  static const std::array<Setting, 4> kSettings{{
      {"thinking", "enum", &ReadThinking,
       [](const config::Backend& backend, int) {
         return boost::json::value(ThinkingName(backend.thinking));
       }},
      {"temperature", "float", &ReadTemperature,
       [](const config::Backend& backend, int) {
         return boost::json::value(static_cast<double>(backend.temperature));
       }},
      {"max_tokens", "int", &ReadMaxTokens,
       [](const config::Backend& backend, int) {
         return backend.max_tokens ? boost::json::value(*backend.max_tokens) : boost::json::value();
       }},
      {"tool_budget", "int", &ReadToolBudget,
       [](const config::Backend&, int budget) { return boost::json::value(budget); }},
  }};
  return kSettings;
}

const Setting* FindSetting(std::string_view name) {
  for (const Setting& setting : Table()) {
    if (name == setting.name) return &setting;
  }
  return nullptr;
}

std::string KnownNames() {
  std::string names;
  for (const Setting& setting : Table()) {
    if (!names.empty()) names += ", ";
    names += setting.name;
  }
  return names;
}

}  // namespace

boost::json::value ParseSetting(const std::string& name, const boost::json::value& raw) {
  const Setting* setting = FindSetting(name);
  if (setting == nullptr) {
    throw RequestRefused("Unknown setting '" + name + "': " + KnownNames());
  }
  if (raw.is_null()) return boost::json::value{};
  return setting->read(raw);
}

std::vector<SettingView> ViewSettings(const config::Backend& agent_backend, int agent_tool_budget,
                                      const Overrides& overrides) {
  std::vector<SettingView> views;
  for (const Setting& setting : Table()) {
    SettingView view;
    view.name = setting.name;
    view.type = setting.type;
    view.agent_value = setting.agent_value(agent_backend, agent_tool_budget);
    const auto it = overrides.find(setting.name);
    if (it != overrides.end() && !it->value().is_null()) {
      view.value = it->value();
      view.source = kSourceSession;
    } else {
      view.value = view.agent_value;
      view.source = kSourceAgent;
    }
    views.push_back(std::move(view));
  }
  return views;
}

config::Backend ApplyOverrides(const config::Backend& agent_backend, const Overrides& overrides) {
  config::Backend backend = agent_backend;
  if (const auto it = overrides.find("thinking");
      it != overrides.end() && it->value().is_string()) {
    backend.thinking = ParseThinkingLevel(boost::json::value_to<std::string>(it->value()));
  }
  if (const auto it = overrides.find("temperature");
      it != overrides.end() && (it->value().is_double() || it->value().is_int64())) {
    backend.temperature =
        it->value().is_double() ? static_cast<float>(it->value().as_double())
                                : static_cast<float>(it->value().as_int64());
  }
  if (const auto it = overrides.find("max_tokens");
      it != overrides.end() && it->value().is_int64()) {
    backend.max_tokens = static_cast<int>(it->value().as_int64());
  }
  return backend;
}

int EffectiveToolBudget(int agent_budget, const Overrides& overrides) {
  const auto it = overrides.find("tool_budget");
  if (it != overrides.end() && it->value().is_int64()) {
    return static_cast<int>(it->value().as_int64());
  }
  return agent_budget;
}

}  // namespace pu::session
