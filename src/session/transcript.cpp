// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/transcript.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>

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
        auto j = nlohmann::json::parse(msg.tool_calls_json);
        for (const auto& tc : j) {
          if (tc.contains("id")) ids.push_back(tc["id"].get<std::string>());
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
      auto j = nlohmann::json::parse(last.tool_calls_json);
      return j.is_array() && !j.empty();
    } catch (...) { return false; }
  }
  return false;
}

nlohmann::json Transcript::Serialize() const {
  nlohmann::json arr = nlohmann::json::array();
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

Transcript Transcript::Deserialize(const nlohmann::json& j) {
  Transcript t;
  if (j.is_array()) {
    for (const auto& item : j) {
      ChatMessage msg;
      msg.id = item.value("id", 0);
      msg.timestamp = item.value("timestamp", "");
      msg.role = item.value("role", "");
      msg.content = item.value("content", "");
      msg.tool_name = item.value("tool_name", "");
      msg.tool_calls_json = item.value("tool_calls_json", "");
      msg.reasoning_content = item.value("reasoning_content", "");
      msg.tool_call_id = item.value("tool_call_id", "");
      t.messages_.push_back(msg);
    }
  }
  return t;
}

} // namespace pu
