// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <nlohmann/json.hpp>
#include "pu/session/artifact.hpp"
#include "pu/session/handoff_receipt.hpp"

namespace pu {

struct Assignment {
  std::string id;
  std::string goal;
  std::string agent_name;
  std::vector<Artifact> seeded_artifacts;  
  std::vector<std::string> constraints;
  int depth = 0;
  std::chrono::steady_clock::time_point created_at;
  std::chrono::steady_clock::time_point deadline;
  std::optional<HandoffReceipt> result;

  static std::string GenerateId();
  bool IsTimeout() const;
  nlohmann::json Serialize() const;
  static Assignment Deserialize(const nlohmann::json& j);
};

} // namespace pu
