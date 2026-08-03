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

void Memory::AddArtifact(const Artifact& artifact) {
  artifacts_.push_back(artifact);
}

std::vector<Artifact> Memory::GetArtifacts() const {
  return artifacts_;
}

void Memory::ClearArtifacts() {
  artifacts_.clear();
}

nlohmann::json Memory::Serialize() const {
  nlohmann::json j;
  nlohmann::json vars_obj = nlohmann::json::object();
  for (const auto& [key, val] : variables_) {
    vars_obj[key] = val;
  }
  j["variables"] = vars_obj;

  nlohmann::json arts_arr = nlohmann::json::array();
  for (const auto& a : artifacts_) {
    arts_arr.push_back(a.Serialize());
  }
  j["artifacts"] = arts_arr;

  return j;
}

Memory Memory::Deserialize(const nlohmann::json& j) {
  Memory m;
  if (j.contains("variables") && j["variables"].is_object()) {
    for (auto& [key, val] : j["variables"].items()) {
      m.variables_[key] = val;
    }
  }
  if (j.contains("artifacts") && j["artifacts"].is_array()) {
    for (const auto& item : j["artifacts"]) {
      m.artifacts_.push_back(Artifact::Deserialize(item));
    }
  } else if (j.contains("facts") && j["facts"].is_array()) {
    // Legacy sessions (pre-v0.4) stored artifacts under the "facts" key.
    for (const auto& item : j["facts"]) {
      m.artifacts_.push_back(Artifact::Deserialize(item));
    }
  }
  return m;
}

} // namespace pu
