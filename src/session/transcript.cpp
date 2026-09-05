// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/transcript.hpp"

#include "pu/json.hpp"

#include <algorithm>
#include <boost/json.hpp>

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
    if (msg.role == "assistant" && !msg.tool_calls_json.empty()) {
      std::vector<std::string> ids;
      try {
        auto j = boost::json::parse(msg.tool_calls_json);
        if (j.is_array()) {
          for (const auto& tc : j.as_array()) {
            if (json::HasKey(tc, "id"))
              ids.push_back(boost::json::value_to<std::string>(tc.at("id")));
          }
        }
      } catch (...) { continue; }

      bool all_found = true;
      for (const auto& id : ids) {
        bool found = false;
        for (size_t j = i + 1; j < messages_.size(); ++j) {
          if (messages_[j].role == "tool" && messages_[j].tool_call_id == id) {
            found = true;
            break;
          }
        }
        if (!found) {
          all_found = false;
          break;
        }
      }
      if (!all_found) {
        tail_start = i;
      }
    }
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
  if (last.role == "assistant" && !last.tool_calls_json.empty()) {
    try {
      auto j = boost::json::parse(last.tool_calls_json);
      return j.is_array() && !j.as_array().empty();
    } catch (...) { return false; }
  }
  return false;
}

boost::json::value Transcript::Serialize() const {
  boost::json::array arr;
  for (const auto& msg : messages_) {
    arr.push_back({
      {"id", msg.id},
      {"timestamp", msg.timestamp},
      {"role", msg.role},
      {"content", msg.content},
      {"tool_name", msg.tool_name},
      {"tool_calls_json", msg.tool_calls_json},
      {"reasoning_content", msg.reasoning_content},
      {"tool_call_id", msg.tool_call_id}
    });
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
      msg.tool_calls_json = json::ValueOrDefault<std::string>(item, "tool_calls_json", "");
      msg.reasoning_content = json::ValueOrDefault<std::string>(item, "reasoning_content", "");
      msg.tool_call_id = json::ValueOrDefault<std::string>(item, "tool_call_id", "");
      t.messages_.push_back(msg);
    }
  }
  return t;
}

} // namespace pu
