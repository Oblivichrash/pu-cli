// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <vector>

#include <boost/json.hpp>

#include "pu/llm/llm_provider.hpp"

namespace pu {

class Transcript {
public:
  void Append(const ChatMessage& msg);
  std::vector<ChatMessage> GetHistory() const;
  std::vector<ChatMessage> Recent(int n) const;
  void Compact(size_t keep_head = 10, size_t keep_tail = 50);
  bool HasPendingToolCalls() const;
  size_t Size() const { return messages_.size(); }

  boost::json::value Serialize() const;
  static Transcript Deserialize(const boost::json::value& j);

private:
  std::vector<ChatMessage> messages_;
};

} // namespace pu
