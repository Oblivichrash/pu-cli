// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/memory.hpp"

#include "pu/json.hpp"

namespace pu {

void Memory::SetVar(const std::string& key, const boost::json::value& value) {
  variables_[key] = value;
}

std::optional<boost::json::value> Memory::GetVar(const std::string& key) const {
  auto it = variables_.find(key);
  if (it == variables_.end()) return std::nullopt;
  return std::optional<boost::json::value>(it->second);
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

boost::json::value Artifact::Serialize() const {
  return boost::json::value{
    {"type", static_cast<int>(type)},
    {"content", content},
    {"source", source},
    {"confidence", confidence},
  };
}

Artifact Artifact::Deserialize(const boost::json::value& j) {
  Artifact a;
  a.type = static_cast<Type>(
      json::ValueOrDefault<int>(j, "type", static_cast<int>(Type::kOther)));
  a.content = json::ValueOrDefault<std::string>(j, "content", "");
  a.source = json::ValueOrDefault<std::string>(j, "source", "");
  a.confidence = json::ValueOrDefault<double>(j, "confidence", 1.0);
  return a;
}

boost::json::value Memory::Serialize() const {
  boost::json::value j = boost::json::object{};
  boost::json::value vars_obj = boost::json::object{};
  for (const auto& [key, val] : variables_) {
    vars_obj.as_object()[key] = val;
  }
  j.as_object()["variables"] = vars_obj;

  boost::json::array arts_arr;
  for (const auto& a : artifacts_) {
    arts_arr.push_back(a.Serialize());
  }
  j.as_object()["artifacts"] = arts_arr;

  return j;
}

Memory Memory::Deserialize(const boost::json::value& j) {
  Memory m;
  if (json::HasKey(j, "variables") && j.at("variables").is_object()) {
    for (const auto& kv : j.at("variables").as_object()) {
      m.variables_[std::string(kv.key())] = kv.value();
    }
  }
  if (json::HasKey(j, "artifacts") && j.at("artifacts").is_array()) {
    for (const auto& item : j.at("artifacts").as_array()) {
      m.artifacts_.push_back(Artifact::Deserialize(item));
    }
  } else if (json::HasKey(j, "facts") && j.at("facts").is_array()) {
    // Legacy sessions (pre-v0.4) stored artifacts under the "facts" key.
    for (const auto& item : j.at("facts").as_array()) {
      m.artifacts_.push_back(Artifact::Deserialize(item));
    }
  }
  return m;
}

} // namespace pu
