// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <map>
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <nlohmann/json.hpp>

namespace pu {

struct BranchNode {
  std::string id;
  std::string owner_id;        // one person per branch
  std::string parent_id;
  std::string head_turn_hash;  // snapshot pointer (simplified)
  bool is_merged = false;
  std::chrono::system_clock::time_point created_at;
  std::string description;
};

class RevisionGraph {
public:
  std::string Fork(const std::string& parent_id, const std::string& owner_id,
                     const std::string& description = "");
  bool Merge(const std::string& child_id, const std::string& target_id);
  std::optional<BranchNode> GetBranch(const std::string& id) const;
  std::vector<BranchNode> ListBranches() const;
  size_t PruneMerged();

  nlohmann::json Serialize() const;
  static RevisionGraph Deserialize(const nlohmann::json& j);

private:
  std::map<std::string, BranchNode> branches_;
  std::string root_id_;
  std::string GenerateId() const;
};

} // namespace pu
