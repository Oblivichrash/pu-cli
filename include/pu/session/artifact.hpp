// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string>
#include <nlohmann/json.hpp>

namespace pu {

struct Artifact {
  enum Type { kFilePath, kErrorMsg, kFunctionName, kUserPreference,
        kCodeSnippet, kUrl, kCommand, kOther };

  Type type = kOther;
  std::string content;
  std::string source;
  double confidence = 1.0;

  // Thin wrappers for backward compatibility with existing callers.
  nlohmann::json Serialize() const { return nlohmann::json(*this); }
  static Artifact Deserialize(const nlohmann::json& j) { return j.get<Artifact>(); }

  NLOHMANN_DEFINE_TYPE_INTRUSIVE(Artifact, type, content, source, confidence)
};

} // namespace pu
