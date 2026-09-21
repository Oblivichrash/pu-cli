// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <map>
#include <vector>

#include <boost/json.hpp>

#include "pu/context/message.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu {

// FROZEN: the public surface does not change. Storage behind it is the
// MessageNode DAG (include/pu/context/message.hpp); the compatibility seam is
// the ChatMessage view this class still renders.
class Transcript {
public:
  void Append(const ChatMessage& msg);
  std::vector<ChatMessage> GetHistory() const;
  void Compact(size_t keep_head = 10, size_t keep_tail = 50);
  bool HasPendingToolCalls() const;
  size_t Size() const;

  boost::json::value Serialize() const;
  static Transcript Deserialize(const boost::json::value& j);

private:
  const context::MessageNode* Find(const context::MessageId& id) const;
  // Nodes from the root to the current leaf, in order.
  std::vector<const context::MessageNode*> Chain() const;
  void MarkToolCallCompleted(const std::string& tool_call_id);

  std::map<context::MessageId, context::MessageNode> nodes_;
  context::MessageId leaf_;
};

} // namespace pu
