// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/transcript.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>

namespace pu {

void Transcript::Append(const ChatMessage& msg) {
  messages_.push_back(msg);
  if (messages_.size() > COMPACT_KEEP_HEAD + COMPACT_KEEP_TAIL) {
    Compact();
  }
}

std::vector<ChatMessage> Transcript::GetHistory() const {
  return messages_;
}

std::vector<ChatMessage> Transcript::Recent(int n) const {
  if (n <= 0) return {};
  if (static_cast<size_t>(n) >= messages_.size()) return messages_;
  return std::vector<ChatMessage>(messages_.end() - n, messages_.end());
}

void Transcript::Compact() {
  const size_t keep_first = COMPACT_KEEP_HEAD;
  const size_t keep_last = COMPACT_KEEP_TAIL;

  if (messages_.size() <= keep_first + keep_last) return;

  // Determine tail start, then walk backward to ensure we don't truncate
  // an assistant message with tool_calls before its matching tool responses.
  size_t tail_start = messages_.size() - keep_last;

  // Scan from tail_start backward up to keep_first to include any
  // incomplete tool-call sequences.
  for (size_t i = tail_start; i > keep_first; --i) {
    const auto& msg = messages_[i];
    if (msg.role == "assistant" && !msg.tool_calls_json.empty()) {
      // Collect all tool_call_id from this assistant's tool_calls.
      std::vector<std::string> ids;
      try {
        auto j = nlohmann::json::parse(msg.tool_calls_json);
        for (const auto& tc : j) {
          if (tc.contains("id")) ids.push_back(tc["id"].get<std::string>());
        }
      } catch (...) {
        continue; // If parsing fails, we cannot verify; skip.
      }

      // Check that every id has a corresponding tool message after i.
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

      // If missing a tool response, extend tail_start to include this assistant.
      if (!all_found) {
        tail_start = i;
        // Continue scanning backward because earlier messages may also be incomplete.
      }
    }
  }

  // Build compressed vector.
  std::vector<ChatMessage> compressed;
  compressed.reserve(keep_first + 1 + (messages_.size() - tail_start));

  compressed.insert(compressed.end(), messages_.begin(), messages_.begin() + keep_first);

  // Only insert summary if we actually omitted something.
  if (tail_start > keep_first) {
    ChatMessage summary;
    summary.id = static_cast<int>(compressed.size()) + 1;
    summary.timestamp = "";
    summary.role = "system";
    summary.content = "[Compressed: " + std::to_string(tail_start - keep_first) + " messages omitted]";
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
    } catch (...) {
      return false;
    }
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

}  // namespace pu
