// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <memory>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "pu/session/transcript.hpp"

namespace pu {

class Workspace {
public:
  Workspace() = default;

  void Append(const ChatMessage& msg);
  void Append(const std::string& role, const std::string& content);
  std::vector<ChatMessage> GetHistory() const;
  size_t HistorySize() const;
  bool HasPendingToolCalls() const;

  // The stored conversation, for a caller that renders its own view of it.
  const context::MessageGraph& GetGraph() const { return transcript_.GetGraph(); }

  void ClearHistory();

  boost::json::value Serialize() const;
  static std::shared_ptr<Workspace> Deserialize(const boost::json::value& j);

private:
  Transcript transcript_;
};

} // namespace pu
