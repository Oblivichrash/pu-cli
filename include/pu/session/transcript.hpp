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
  bool HasPendingToolCalls() const;
  size_t Size() const;

  // The stored conversation, for a caller that renders its own view of it.
  const context::MessageGraph& GetGraph() const { return graph_; }

  boost::json::value Serialize() const;
  // Returns false for a value that is not DAG storage, so the caller can tell a
  // foreign layout from an empty conversation.
  static bool Deserialize(const boost::json::value& j, Transcript& out);

private:
  context::MessageGraph graph_;
};

} // namespace pu
