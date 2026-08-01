// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <map>
#include <optional>
#include <vector>
#include <nlohmann/json.hpp>
#include "pu/session/artifact.hpp"

namespace pu {

class Memory {
public:
  void SetVar(const std::string& key, const nlohmann::json& value);
  std::optional<nlohmann::json> GetVar(const std::string& key) const;
  bool HasVar(const std::string& key) const;
  void RemoveVar(const std::string& key);

  void AddArtifact(const Artifact& artifact);
  std::vector<Artifact> GetArtifacts() const;
  void ClearArtifacts();

  nlohmann::json Serialize() const;
  static Memory Deserialize(const nlohmann::json& j);

private:
  std::map<std::string, nlohmann::json> variables_;
  std::vector<Artifact> artifacts_;
};

} // namespace pu
