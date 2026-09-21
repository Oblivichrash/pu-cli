// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <vector>

#include <boost/json.hpp>

#include "pu/context/graph.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu {

// FROZEN: the public surface does not change. Storage behind it is the
// MessageGraph (include/pu/context/graph.hpp); the compatibility seam is the
// ChatMessage view this class renders from it.
class Transcript {
public:
  void Append(const ChatMessage& msg);
  std::vector<ChatMessage> GetHistory() const;
  void Compact(size_t keep_head = 10, size_t keep_tail = 50);
  bool HasPendingToolCalls() const;
  size_t Size() const;

  // The stored conversation, for a caller that renders its own view of it.
  const context::MessageGraph& GetGraph() const { return graph_; }

  boost::json::value Serialize() const;
  static Transcript Deserialize(const boost::json::value& j);

private:
  context::MessageGraph graph_;
};

} // namespace pu
