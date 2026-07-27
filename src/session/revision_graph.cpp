// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/revision_graph.hpp"
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

namespace pu {

std::string RevisionGraph::GenerateId() const {
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ss;
  ss << std::put_time(std::gmtime(&in_time_t), "%Y%m%d%H%M%S");

  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<> dis(1000, 9999);
  ss << "-" << dis(gen);

  return "branch-" + ss.str();
}

std::string RevisionGraph::Fork(const std::string& parent_id, const std::string& owner_id,
                const std::string& description) {
  std::string id = GenerateId();
  BranchNode node;
  node.id = id;
  node.owner_id = owner_id;
  node.parent_id = parent_id;
  node.created_at = std::chrono::system_clock::now();
  node.description = description;
  node.is_merged = false;

  branches_[id] = node;

  if (root_id_.empty()) {
    root_id_ = id;
  }

  return id;
}

bool RevisionGraph::Merge(const std::string& child_id, const std::string& target_id) {
  auto it = branches_.find(child_id);
  if (it == branches_.end()) return false;

  auto target_it = branches_.find(target_id);
  if (target_it == branches_.end()) return false;

  it->second.is_merged = true;
  return true;
}

std::optional<BranchNode> RevisionGraph::GetBranch(const std::string& id) const {
  auto it = branches_.find(id);
  if (it == branches_.end()) return std::nullopt;
  return it->second;
}

std::vector<BranchNode> RevisionGraph::ListBranches() const {
  std::vector<BranchNode> result;
  for (const auto& [id, node] : branches_) {
    result.push_back(node);
  }
  return result;
}

size_t RevisionGraph::PruneMerged() {
  size_t count = 0;
  auto it = branches_.begin();
  while (it != branches_.end()) {
    if (it->second.is_merged) {
      it = branches_.erase(it);
      ++count;
    } else {
      ++it;
    }
  }
  return count;
}

nlohmann::json RevisionGraph::Serialize() const {
  nlohmann::json j;
  j["root_id"] = root_id_;
  nlohmann::json branches = nlohmann::json::object();
  for (const auto& [id, node] : branches_) {
    nlohmann::json n;
    n["id"] = node.id;
    n["owner_id"] = node.owner_id;
    n["parent_id"] = node.parent_id;
    n["head_turn_hash"] = node.head_turn_hash;
    n["is_merged"] = node.is_merged;
    n["description"] = node.description;
    auto tt = std::chrono::system_clock::to_time_t(node.created_at);
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&tt), "%Y-%m-%dT%H:%M:%SZ");
    n["created_at"] = ss.str();
    branches[id] = n;
  }
  j["branches"] = branches;
  return j;
}

RevisionGraph RevisionGraph::Deserialize(const nlohmann::json& j) {
  RevisionGraph g;
  g.root_id_ = j.value("root_id", "");
  if (j.contains("branches") && j["branches"].is_object()) {
    for (auto& [id, node_j] : j["branches"].items()) {
      BranchNode n;
      n.id = node_j.value("id", id);
      n.owner_id = node_j.value("owner_id", "");
      n.parent_id = node_j.value("parent_id", "");
      n.head_turn_hash = node_j.value("head_turn_hash", "");
      n.is_merged = node_j.value("is_merged", false);
      n.description = node_j.value("description", "");
      g.branches_[id] = n;
    }
  }
  return g;
}

} // namespace pu
