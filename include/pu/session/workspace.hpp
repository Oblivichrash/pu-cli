// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <memory>
#include <vector>

#include <boost/json.hpp>

#include "pu/session/transcript.hpp"
#include "pu/session/memory.hpp"

namespace pu {

class Workspace {
public:
  Workspace() = default;

  void Append(const ChatMessage& msg);
  void Append(const std::string& role, const std::string& content);
  std::vector<ChatMessage> GetHistory() const;
  std::vector<ChatMessage> Recent(int n) const;
  size_t HistorySize() const;
  void Compact(size_t keep_head = 10, size_t keep_tail = 50);
  bool HasPendingToolCalls() const;

  void ClearHistory();

  void SetVar(const std::string& key, const boost::json::value& value);
  std::optional<boost::json::value> GetVar(const std::string& key) const;
  bool HasVar(const std::string& key) const;
  void RemoveVar(const std::string& key);

  void AddArtifact(const Artifact& artifact);
  std::vector<Artifact> GetArtifacts() const;
  void ClearArtifacts();

  boost::json::value Serialize() const;
  static std::shared_ptr<Workspace> Deserialize(const boost::json::value& j);

private:
  std::unique_ptr<Transcript> transcript_;
  std::unique_ptr<Memory> memory_;
};

} // namespace pu
