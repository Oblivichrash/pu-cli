// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/core/fact.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pu::core {

struct SummaryReport {
  enum class Status {
    kCompleted,
    kFailed,
    kCancelled,
    kTimeout
  };

  Status status = Status::kCompleted;
  std::string summary;
  FactList key_discoveries;
  std::vector<std::string> unresolved;
  std::chrono::milliseconds duration = std::chrono::milliseconds::zero();

  bool IsSuccess() const { return status == Status::kCompleted; }
  bool IsFinal() const { return status != Status::kCompleted || !summary.empty(); }
};

struct Delegation {
  std::string id;
  std::string goal;
  std::string agent_name;
  FactList seeded_facts;
  std::unordered_map<std::string, std::string> constraints;
  int depth = 0;
  std::chrono::steady_clock::time_point created_at;
  std::chrono::steady_clock::time_point deadline;
  std::optional<SummaryReport> result;

  Delegation() = default;

  Delegation(std::string goal_, std::string agent_name_, FactList facts_ = {}, int depth_ = 0)
      : goal(std::move(goal_)),
        agent_name(std::move(agent_name_)),
        seeded_facts(std::move(facts_)),
        depth(depth_) {}

  static std::string GenerateId();
  bool IsTimeout() const;
};

}  // namespace pu::core
