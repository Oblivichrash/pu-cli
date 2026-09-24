// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The stored conversation: nodes keyed by id, with a current leaf marking the
// position. Order is derived from the parent link, so identity never depends on
// position.
//
// Not thread-safe. Caller must serialize access.

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "pu/context/message.hpp"

#include <boost/json.hpp>

namespace pu::context {

// The persisted layout of the context model. Bumped when the shape changes in a
// way an older reader cannot interpret; there is no reader for older values.
inline constexpr int kSchemaVersion = 4;

class MessageGraph {
 public:
  const MessageId& leaf() const { return leaf_; }

  std::size_t Size() const { return nodes_.size(); }

  const MessageNode* Find(const MessageId& id) const {
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
  }

  // Nodes from the root to the leaf, in order. The only ordering the graph
  // defines, because there is no positional index to fall back on.
  std::vector<const MessageNode*> Chain() const { return ChainFrom(leaf_); }

  // The same walk from any node, which is what lets a caller render a view
  // other than the current position.
  std::vector<const MessageNode*> ChainFrom(const MessageId& id) const {
    std::vector<const MessageNode*> chain;
    const MessageNode* node = Find(id);
    while (node != nullptr) {
      chain.push_back(node);
      node = node->parent.empty() ? nullptr : Find(node->parent);
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
  }

  // Appends after the current leaf, linking it and moving the leaf along. The
  // append is also where a replaced turn disappears: whatever the new leaf
  // cannot reach is dropped, so editing a message ends up storing what sending
  // the new text from the start would have stored.
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

  // Moves the leaf back to a node that is already stored, so the next append
  // replaces the turns after it rather than extending them. Nothing is removed
  // here: the turns after the new position stay until an append replaces them,
  // and that append is what drops them. An empty id means "before everything".
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

  // Nodes plus the leaf, which is everything needed to resume. Written as an
  // object rather than a list because a position is not derivable from order.
  boost::json::value Serialize() const;

  // Reads what Serialize wrote. Returns false for anything else, including a
  // list of messages, so a file from another layout is refused rather than
  // half-read.
  static bool Deserialize(const boost::json::value& value, MessageGraph& out);

 private:
  const MessageNode& Add(MessageNode node) {
    const MessageId id = node.id;
    const MessageNode& stored = nodes_.emplace(id, std::move(node)).first->second;
    CompleteAnsweredRecord(stored.payload);
    return stored;
  }

  // Keeps the nodes the leaf still reaches and erases the rest. Only a step back
  // followed by an append leaves anything unreachable, and the erase happens on
  // that append rather than on the step back, so the abandoned turns survive
  // until something replaces them.
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

  // Marks the record that a receipt answers as completed, searching back from
  // the leaf because a receipt need not sit next to its request.
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
  // completed and no caller has to remember to say so. A load needs no such step:
  // the status was already settled when the file was written.
  void CompleteAnsweredRecord(const MessagePayload& payload) {
    const auto* receipt = std::get_if<ToolPayload>(&payload);
    if (receipt != nullptr) CompleteFor(receipt->tool_call_id);
  }

  std::map<MessageId, MessageNode> nodes_;
  MessageId leaf_;
};

}  // namespace pu::context
