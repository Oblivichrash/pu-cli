// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/conversation.hpp"

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pu/core/fact.hpp"

namespace pu::core {

using json = nlohmann::json;

class Context : public std::enable_shared_from_this<Context> {
 public:
  Context() = default;
  explicit Context(std::string id);

  const std::string& GetId() const { return id_; }
  void SetId(const std::string& id) { id_ = id; }

  void Append(const ChatMessage& msg);
  void Append(const std::string& role, const std::string& content);
  std::vector<ChatMessage> GetHistory() const { return history_; }
  std::vector<ChatMessage> Recent(int n) const;
  size_t HistorySize() const { return history_.size(); }
  void ClearHistory();

  void SetVar(const std::string& key, const json& value);
  std::optional<json> GetVar(const std::string& key) const;
  bool HasVar(const std::string& key) const;
  void RemoveVar(const std::string& key);
  std::unordered_map<std::string, json> GetAllVars() const { return vars_; }

  void AddFact(const Fact& fact);
  void AddFacts(const FactList& facts);
  FactList GetFacts() const { return facts_; }
  FactList GetFactsByType(Fact::Type type) const;
  void ClearFacts();

  void Compact();

  void Save(const std::filesystem::path& path) const;
  static std::shared_ptr<Context> Load(const std::filesystem::path& path);
  static std::shared_ptr<Context> LoadOrCreate(const std::filesystem::path& path);

  enum class State { kActive, kMerged, kAbandoned };

  /// Create an isolated child context that inherits all data from parent
  /// but has independent history.
  std::shared_ptr<Context> Fork(const std::string& branch_name);

  /// Merge a child context back into this parent.
  /// Creates a new merge context that combines histories.
  /// Returns the merge context.
  std::shared_ptr<Context> Merge(const std::shared_ptr<Context>& child,
                                  const std::string& message);

  /// Get estimated token count (rough: sum of content lengths / 4)
  size_t GetTokenCount() const;

  

  std::shared_ptr<Context> GetParent() const { return parent_.lock(); }
  const std::vector<std::shared_ptr<Context>>& GetChildren() const { return children_; }
  const std::string& GetBranchName() const { return branch_name_; }
  State GetState() const { return state_; }
  bool IsMergeCommit() const { return is_merge_commit_; }
  std::vector<std::shared_ptr<Context>> GetMergeParents() const;



  size_t RemoveMergedChildren();

 private:
  std::string id_;
  std::vector<ChatMessage> history_;
  std::unordered_map<std::string, json> vars_;
  FactList facts_;
  size_t max_history_size_ = 1000;


  std::weak_ptr<Context> parent_;
  std::vector<std::shared_ptr<Context>> children_;
  std::string branch_name_ = "main";


  bool is_merge_commit_ = false;
  std::vector<std::weak_ptr<Context>> merge_parents_;
  std::optional<std::string> merge_message_;


  State state_ = State::kActive;
};

}  // namespace pu::core
