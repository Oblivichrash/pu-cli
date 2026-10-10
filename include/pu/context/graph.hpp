// SPDX-License-Identifier: GPL-3.0-only
#pragma once

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

  bool RewindTo(const MessageId& id) {
    if (!id.empty() && nodes_.find(id) == nodes_.end()) return false;
    leaf_ = id;
    return true;
  }

  bool LeafHasUnfinishedToolCalls() const {
    const MessageNode* node = Find(leaf_);
    return node != nullptr && HasUnfinishedToolCalls(*node);
  }

  boost::json::value Serialize() const;

  static bool Deserialize(const boost::json::value& value, MessageGraph& out);

 private:
  const MessageNode* Find(const MessageId& id) const {
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
  }

  bool LinksResolve() const {
    enum class Mark { kUnvisited, kOnPath, kSound };
    std::map<MessageId, Mark> marks;

    for (const auto& entry : nodes_) {
      MessageId current = entry.first;
      std::vector<MessageId> path;
      while (!current.empty()) {
        const auto it = nodes_.find(current);
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

  void CompleteAnsweredRecord(const MessagePayload& payload) {
    const auto* receipt = std::get_if<ToolPayload>(&payload);
    if (receipt != nullptr) CompleteFor(receipt->tool_call_id);
  }

  std::map<MessageId, MessageNode> nodes_;
  MessageId leaf_;
};

}  // namespace pu::context
