// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>
#include <filesystem>
#include "pu/session/transcript.hpp"
#include "pu/session/memory.hpp"

namespace pu {

class Workspace {
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
  void Compact(size_t keep_head = 10, size_t keep_tail = 50);
  bool HasPendingToolCalls() const;

  void ClearHistory();

  // Delegate to Memory
  void SetVar(const std::string& key, const nlohmann::json& value);
  std::optional<nlohmann::json> GetVar(const std::string& key) const;
  bool HasVar(const std::string& key) const;
  void RemoveVar(const std::string& key);

  void AddArtifact(const Artifact& artifact);
  std::vector<Artifact> GetArtifacts() const;
  void ClearArtifacts();

  // Serialization (compatible with old Context JSON structure)
  nlohmann::json Serialize() const;
  void Save(const std::filesystem::path& path) const;
  static std::shared_ptr<Workspace> Load(const std::filesystem::path& path);
  static std::shared_ptr<Workspace> Deserialize(const nlohmann::json& j);

private:
  std::string id_;

  std::unique_ptr<Transcript> transcript_;
  std::unique_ptr<Memory> memory_;
};

} // namespace pu
