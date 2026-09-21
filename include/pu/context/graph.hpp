// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The stored conversation: nodes keyed by id, with a current leaf marking the
// position. Order is derived from parent links, so identity never depends on
// position and a node can be referenced from more than one place.
//
// Not thread-safe. Caller must serialize access.

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "pu/context/message.hpp"

namespace pu::context {

class MessageGraph {
 public:
  const MessageId& leaf() const { return leaf_; }

  std::size_t Size() const { return nodes_.size(); }
  bool empty() const { return nodes_.empty(); }

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
      node = node->parents.empty() ? nullptr : Find(node->parents.front());
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
  }

  // Appends after the current leaf, linking it and moving the leaf along.
  const MessageNode& AppendAfterLeaf(MessagePayload payload,
                                     std::string timestamp = {}) {
    MessageNode node = MakeNode(std::move(payload));
    node.timestamp = std::move(timestamp);
    if (!leaf_.empty()) node.parents.push_back(leaf_);

    const MessageId id = node.id;
    const MessageNode& stored = Add(std::move(node));
    leaf_ = id;
    return stored;
  }

  // True when the last node still has a tool call in flight.
  bool LeafHasUnfinishedToolCalls() const {
    const MessageNode* node = Find(leaf_);
    return node != nullptr && HasUnfinishedToolCalls(*node);
  }

 private:
  const MessageNode& Add(MessageNode node) {
    const MessageId id = node.id;
    const MessageNode& stored = nodes_.emplace(id, std::move(node)).first->second;
    CompleteAnsweredRecord(stored.payload);
    return stored;
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
      id = it->second.parents.empty() ? MessageId{} : it->second.parents.front();
    }
  }

  // Every insertion goes through here, so a receipt always answers its record
  // and no caller has to remember to say so.
  void CompleteAnsweredRecord(const MessagePayload& payload) {
    const auto* receipt = std::get_if<ToolPayload>(&payload);
    if (receipt != nullptr) CompleteFor(receipt->tool_call_id);
  }

  std::map<MessageId, MessageNode> nodes_;
  MessageId leaf_;
};

}  // namespace pu::context
