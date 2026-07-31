// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string>
#include <vector>
#include <chrono>
#include "pu/session/artifact.hpp"

namespace pu {

struct HandoffReceipt {
  enum Status { kCompleted, kFailed, kCancelled, kTimeout };

  Status status = kCompleted;
  std::string summary;
  std::vector<Artifact> key_discoveries;
  std::vector<std::string> unresolved;
  std::chrono::milliseconds duration;

  nlohmann::json Serialize() const;
  static HandoffReceipt Deserialize(const nlohmann::json& j);
};

} // namespace pu
