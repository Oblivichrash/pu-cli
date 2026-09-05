// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace pu {

struct Artifact {
  enum Type { kFilePath, kErrorMsg, kFunctionName, kUserPreference,
        kCodeSnippet, kUrl, kCommand, kOther };

  Type type = kOther;
  std::string content;
  std::string source;
  double confidence = 1.0;

  // Boost.JSON equivalents of the former NLOHMANN_DEFINE_TYPE_INTRUSIVE macro.
  // The enum is stored as its integer value to stay compatible with older
  // session files produced by nlohmann::json.
  boost::json::value Serialize() const;
  static Artifact Deserialize(const boost::json::value& j);
};

class Memory {
public:
  void SetVar(const std::string& key, const boost::json::value& value);
  std::optional<boost::json::value> GetVar(const std::string& key) const;
  bool HasVar(const std::string& key) const;
  void RemoveVar(const std::string& key);

  void AddArtifact(const Artifact& artifact);
  std::vector<Artifact> GetArtifacts() const;
  void ClearArtifacts();

  boost::json::value Serialize() const;
  static Memory Deserialize(const boost::json::value& j);

private:
  std::map<std::string, boost::json::value> variables_;
  std::vector<Artifact> artifacts_;
};

} // namespace pu
