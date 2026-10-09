// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Nodes keyed by id with a current leaf marking the position; order is derived from the
// parent links, so identity never depends on position. Not thread-safe.

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "pu/context/message.hpp"

#include <boost/json.hpp>

namespace pu::context {

class MessageGraph {
 public:
  const MessageId& leaf() const { return leaf_; }

  std::size_t Size() const { return nodes_.size(); }

  // Root to leaf, in order: the only ordering the graph defines.
  std::vector<const MessageNode*> Chain() const {
    std::vector<const MessageNode*> chain;
    const MessageNode* node = Find(leaf_);
    while (node != nullptr) {
      chain.push_back(node);
      node = node->parent.empty() ? nullptr : Find(node->parent);
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
  }

  // Appends after the current leaf and moves it along, dropping whatever the new leaf
  // cannot reach: editing a message stores what resending from the start would have.
  const MessageNode& AppendAfterLeaf(MessagePayload payload, std::string timestamp = {}) {
    MessageNode node = MakeNode(std::move(payload));
    node.timestamp = std::move(timestamp);
    if (!leaf_.empty()) node.parent = leaf_;

    const MessageId id = node.id;
    const MessageNode& stored = Add(std::move(node));
    leaf_ = id;
    DropUnreachable();
    return stored;
  }

  // Moves the leaf back so the next append replaces the turns after it; those stay
  // until that append. An empty id means "before everything".
  bool RewindTo(const MessageId& id) {
    if (!id.empty() && nodes_.find(id) == nodes_.end()) return false;
    leaf_ = id;
    return true;
  }

  // True when the last node still has a tool call in flight.
  bool LeafHasUnfinishedToolCalls() const {
    const MessageNode* node = Find(leaf_);
    return node != nullptr && HasUnfinishedToolCalls(*node);
  }

  // Nodes plus the leaf, as an object: a position is not derivable from order.
  boost::json::value Serialize() const;

  // Reads what Serialize wrote; anything else, a message list included, returns false.
  static bool Deserialize(const boost::json::value& value, MessageGraph& out);

 private:
  const MessageNode* Find(const MessageId& id) const {
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
  }

  // True when every parent names a stored node and no chain returns to one it already
  // passed: each node has one parent, so a chain that repeats has entered a cycle.
  bool LinksResolve() const {
    enum class Mark { kUnvisited, kOnPath, kSound };
    std::map<MessageId, Mark> marks;

    for (const auto& entry : nodes_) {
      MessageId current = entry.first;
      std::vector<MessageId> path;
      while (!current.empty()) {
        const auto it = nodes_.find(current);
        // A parent naming nothing would leave the turns before it out of the chain.
        if (it == nodes_.end()) return false;

        Mark& mark = marks[current];
        if (mark == Mark::kSound) break;
        if (mark == Mark::kOnPath) return false;

        mark = Mark::kOnPath;
        path.push_back(current);
        current = it->second.parent;
      }
      for (const MessageId& id : path) marks[id] = Mark::kSound;
    }
    return true;
  }

  const MessageNode& Add(MessageNode node) {
    const MessageId id = node.id;
    const MessageNode& stored = nodes_.emplace(id, std::move(node)).first->second;
    CompleteAnsweredRecord(stored.payload);
    return stored;
  }

  // Erases what the leaf cannot reach; only a rewind followed by an append leaves any.
  void DropUnreachable() {
    std::vector<MessageId> reachable;
    for (const MessageNode* node : Chain()) reachable.push_back(node->id);

    for (auto it = nodes_.begin(); it != nodes_.end();) {
      if (std::find(reachable.begin(), reachable.end(), it->first) == reachable.end()) {
        it = nodes_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Searches back from the leaf: a receipt need not sit next to its request.
  void CompleteFor(const std::string& tool_call_id) {
    if (tool_call_id.empty()) return;
    MessageId id = leaf_;
    while (!id.empty()) {
      const auto it = nodes_.find(id);
      if (it == nodes_.end()) return;

      if (auto* assistant = std::get_if<AssistantPayload>(&it->second.payload)) {
        for (ToolCallRecord& record : assistant->tool_calls) {
          if (record.id == tool_call_id) {
            record.status = ToolCallStatus::kCompleted;
            return;
          }
        }
      }
      id = it->second.parent;
    }
  }

  // Every appended turn goes through here, so a receipt always marks its record
  // completed; a load needs no such step, the status was settled when it was written.
  void CompleteAnsweredRecord(const MessagePayload& payload) {
    const auto* receipt = std::get_if<ToolPayload>(&payload);
    if (receipt != nullptr) CompleteFor(receipt->tool_call_id);
  }

  std::map<MessageId, MessageNode> nodes_;
  MessageId leaf_;
};

}  // namespace pu::context
