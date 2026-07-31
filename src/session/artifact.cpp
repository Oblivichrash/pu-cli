// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/artifact.hpp"

namespace pu {

nlohmann::json Artifact::Serialize() const {
  return {
    {"type", static_cast<int>(type)},
    {"content", content},
    {"source", source},
    {"confidence", confidence}
  };
}

Artifact Artifact::Deserialize(const nlohmann::json& j) {
  Artifact a;
  a.type = static_cast<Type>(j.value("type", 0));
  a.content = j.value("content", "");
  a.source = j.value("source", "");
  a.confidence = j.value("confidence", 1.0);
  return a;
}

} // namespace pu
