// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/memory.hpp"

namespace pu {

void Memory::SetVar(const std::string& key, const nlohmann::json& value) {
  variables_[key] = value;
}

std::optional<nlohmann::json> Memory::GetVar(const std::string& key) const {
  auto it = variables_.find(key);
  if (it == variables_.end()) return std::nullopt;
  return std::optional<nlohmann::json>(it->second);
}

bool Memory::HasVar(const std::string& key) const {
  return variables_.find(key) != variables_.end();
}

void Memory::RemoveVar(const std::string& key) {
  variables_.erase(key);
}

nlohmann::json Memory::Serialize() const {
  nlohmann::json j;
  nlohmann::json vars_obj = nlohmann::json::object();
  for (const auto& [key, val] : variables_) {
    vars_obj[key] = val;
  }
  j["variables"] = vars_obj;

  return j;
}

Memory Memory::Deserialize(const nlohmann::json& j) {
  Memory m;
  if (j.contains("variables") && j["variables"].is_object()) {
    for (auto& [key, val] : j["variables"].items()) {
      m.variables_[key] = val;
    }
  }
  // Legacy "artifacts"/"facts" keys from pre-v0.4 sessions are ignored; old
  // files still load, they just carry no memory variables.
  return m;
}

} // namespace pu
