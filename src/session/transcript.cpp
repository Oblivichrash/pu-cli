// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/transcript.hpp"

#include "pu/core/json.hpp"

#include <algorithm>

namespace pu {

void Transcript::Append(const ChatMessage& msg) {
  messages_.push_back(msg);
}

std::vector<ChatMessage> Transcript::GetHistory() const {
  return messages_;
}

std::vector<ChatMessage> Transcript::Recent(int n) const {
  if (n <= 0) return {};
  if (static_cast<size_t>(n) >= messages_.size()) return messages_;
  return std::vector<ChatMessage>(messages_.end() - n, messages_.end());
}

void Transcript::Compact(size_t keep_head, size_t keep_tail) {
  if (messages_.size() <= keep_head + keep_tail) return;

  size_t tail_start = messages_.size() - keep_tail;
  for (size_t i = tail_start; i > keep_head; --i) {
    const auto& msg = messages_[i];
    if (msg.role != "assistant" || !msg.HasToolCalls()) continue;

    // Keep every tool call together with the tool result it produced.
    bool all_matched = true;
    for (const auto& call : msg.tool_calls.as_array()) {
      const std::string id = json::ValueOrDefault<std::string>(call, "id", "");
      if (id.empty()) continue;
      bool matched = false;
      for (size_t j = i + 1; j < messages_.size(); ++j) {
        if (messages_[j].role == "tool" && messages_[j].tool_call_id == id) {
          matched = true;
          break;
        }
      }
      if (!matched) {
        all_matched = false;
        break;
      }
    }
    if (!all_matched) tail_start = i;
  }

  std::vector<ChatMessage> compressed;
  compressed.reserve(keep_head + 1 + (messages_.size() - tail_start));
  compressed.insert(compressed.end(), messages_.begin(), messages_.begin() + keep_head);
  if (tail_start > keep_head) {
    ChatMessage summary;
    summary.id = static_cast<int>(compressed.size()) + 1;
    summary.timestamp = "";
    summary.role = "system";
    summary.content = "[Compressed: " + std::to_string(tail_start - keep_head) + " messages omitted]";
    compressed.push_back(summary);
  }
  compressed.insert(compressed.end(), messages_.begin() + tail_start, messages_.end());
  messages_ = std::move(compressed);
}

bool Transcript::HasPendingToolCalls() const {
  if (messages_.empty()) return false;
  const auto& last = messages_.back();
  return last.role == "assistant" && last.HasToolCalls();
}

boost::json::value Transcript::Serialize() const {
  boost::json::array arr;
  for (const auto& msg : messages_) {
    boost::json::object entry = {
      {"id", msg.id},
      {"timestamp", msg.timestamp},
      {"role", msg.role},
      {"content", msg.content},
      {"tool_name", msg.tool_name},
      {"reasoning_content", msg.reasoning_content},
      {"tool_call_id", msg.tool_call_id}
    };
    if (msg.HasToolCalls()) entry["tool_calls"] = msg.tool_calls;
    arr.push_back(std::move(entry));
  }
  return arr;
}

Transcript Transcript::Deserialize(const boost::json::value& j) {
  Transcript t;
  if (j.is_array()) {
    for (const auto& item : j.as_array()) {
      ChatMessage msg;
      msg.id = json::ValueOrDefault<int>(item, "id", 0);
      msg.timestamp = json::ValueOrDefault<std::string>(item, "timestamp", "");
      msg.role = json::ValueOrDefault<std::string>(item, "role", "");
      msg.content = json::ValueOrDefault<std::string>(item, "content", "");
      msg.tool_name = json::ValueOrDefault<std::string>(item, "tool_name", "");
        msg.tool_calls = json::ValueOrDefault<boost::json::value>(
          item, "tool_calls", boost::json::value(nullptr));
      msg.reasoning_content = json::ValueOrDefault<std::string>(item, "reasoning_content", "");
      msg.tool_call_id = json::ValueOrDefault<std::string>(item, "tool_call_id", "");
      t.messages_.push_back(std::move(msg));
    }
  }
  return t;
}

} // namespace pu
