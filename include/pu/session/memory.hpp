// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace pu {

struct Artifact {
  enum Type { kFilePath, kErrorMsg, kFunctionName, kUserPreference,
        kCodeSnippet, kUrl, kCommand, kOther };

  Type type = kOther;
  std::string content;
  std::string source;
  double confidence = 1.0;

  nlohmann::json Serialize() const { return nlohmann::json(*this); }
  static Artifact Deserialize(const nlohmann::json& j) { return j.get<Artifact>(); }

  NLOHMANN_DEFINE_TYPE_INTRUSIVE(Artifact, type, content, source, confidence)
};

class Memory {
public:
  void SetVar(const std::string& key, const nlohmann::json& value);
  std::optional<nlohmann::json> GetVar(const std::string& key) const;
  bool HasVar(const std::string& key) const;
  void RemoveVar(const std::string& key);

  nlohmann::json Serialize() const;
  static Memory Deserialize(const nlohmann::json& j);

private:
  std::map<std::string, nlohmann::json> variables_;
  std::vector<Artifact> artifacts_;
};

} // namespace pu
