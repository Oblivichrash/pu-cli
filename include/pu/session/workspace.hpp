// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>
#include <filesystem>
#include "pu/session/transcript.hpp"
#include "pu/session/memory.hpp"
#include "pu/session/revision_graph.hpp"

namespace pu {

class Workspace : public std::enable_shared_from_this<Workspace> {
public:
  Workspace() = default;
  explicit Workspace(const std::string& id);

  const std::string& GetId() const { return id_; }
  void SetId(const std::string& id) { id_ = id; }

  // Delegate to Transcript
  void Append(const ChatMessage& msg);
  void Append(const std::string& role, const std::string& content);
  std::vector<ChatMessage> GetHistory() const;
  std::vector<ChatMessage> Recent(int n) const;
  size_t HistorySize() const;
  void Compact();
  bool HasPendingToolCalls() const;
  
  // New method for clearing history
  void ClearHistory();

  // Delegate to Memory
  void SetVar(const std::string& key, const nlohmann::json& value);
  std::optional<nlohmann::json> GetVar(const std::string& key) const;
  bool HasVar(const std::string& key) const;
  void RemoveVar(const std::string& key);

  void AddArtifact(const Artifact& artifact);
  std::vector<Artifact> GetArtifacts() const;
  void ClearArtifacts();

  // Branch operations
  std::shared_ptr<Workspace> Fork(const std::string& branch_name = "");
  std::shared_ptr<Workspace> Merge(const std::shared_ptr<Workspace>& child,
                                     const std::string& message);
  std::shared_ptr<Workspace> GetParent() const { return parent_.lock(); }
  const std::vector<std::shared_ptr<Workspace>>& GetChildren() const { return children_; }
  std::vector<std::shared_ptr<Workspace>> GetMergeParents() const;

  // State
  enum class State { kActive, kMerged, kAbandoned };
  State GetState() const { return state_; }
  void SetState(State s) { state_ = s; }
  std::string GetBranchName() const { return branch_name_; }
  bool IsMergeCommit() const { return is_merge_commit_; }

  size_t RemoveMergedChildren();
  size_t GetTokenCount() const;

  // Serialization (compatible with old Context JSON structure)
  nlohmann::json Serialize() const;
  void Save(const std::filesystem::path& path) const;
  static std::shared_ptr<Workspace> Load(const std::filesystem::path& path);
  static std::shared_ptr<Workspace> LoadOrCreate(const std::filesystem::path& path);
  static std::shared_ptr<Workspace> Deserialize(const nlohmann::json& j);

private:
  std::string id_;
  std::string branch_name_ = "main";
  State state_ = State::kActive;
  bool is_merge_commit_ = false;
  std::vector<std::weak_ptr<Workspace>> merge_parents_;
  std::weak_ptr<Workspace> parent_;
  std::vector<std::shared_ptr<Workspace>> children_;

  std::unique_ptr<Transcript> transcript_;
  std::unique_ptr<Memory> memory_;
  std::unique_ptr<RevisionGraph> graph_;
};

} // namespace pu
