// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <vector>
#include <nlohmann/json.hpp>
#include "pu/conversation.hpp"  // ChatMessage

namespace pu {

class Transcript {
public:
  void Append(const ChatMessage& msg);
  std::vector<ChatMessage> GetHistory() const;
  std::vector<ChatMessage> Recent(int n) const;
  void Compact();
  bool HasPendingToolCalls() const;  // check last assistant msg for unfinished tool_calls
  size_t Size() const { return messages_.size(); }

  nlohmann::json Serialize() const;
  static Transcript Deserialize(const nlohmann::json& j);

private:
  std::vector<ChatMessage> messages_;
  static constexpr size_t COMPACT_KEEP_HEAD = 10;
  static constexpr size_t COMPACT_KEEP_TAIL = 50;
};

} // namespace pu
