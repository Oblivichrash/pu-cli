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

  nlohmann::json Serialize() const;
  static Artifact Deserialize(const nlohmann::json& j);
};

} // namespace pu
