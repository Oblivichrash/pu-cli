// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/transcript.hpp"

#include "pu/core/json.hpp"
#include "pu/core/text.hpp"
#include "pu/session/request.hpp"

#include <algorithm>

namespace pu {

namespace {

std::vector<context::ContentPart> ToContent(const std::string& text) {
  std::vector<context::ContentPart> content;
  content.emplace_back(context::TextPart{text});
  return content;
}

context::ToolCallRecord ToRecord(const boost::json::value& call) {
  context::ToolCallRecord record;
  record.id = json::ValueOrDefault<std::string>(call, "id", "");
  if (!json::HasKey(call, "function")) return record;

  const boost::json::value& function = call.at("function");
  record.name = json::ValueOrDefault<std::string>(function, "name", "");
  record.arguments = json::ValueOrDefault<boost::json::value>(
      function, "arguments", boost::json::object{});
  return record;
}

context::MessagePayload ToPayload(const ChatMessage& msg) {
  std::vector<context::ContentPart> content = ToContent(msg.content);

  if (msg.role == "assistant") {
    context::AssistantPayload assistant;
    assistant.content = std::move(content);
    if (!msg.reasoning_content.empty()) {
      // The legacy field is the reasoning text as the provider sent it.
      assistant.reasoning = context::Reasoning{"", "", msg.reasoning_content};
    }
    if (msg.tool_calls.is_array()) {
      for (const boost::json::value& call : msg.tool_calls.as_array()) {
        assistant.tool_calls.push_back(ToRecord(call));
      }
    }
    return assistant;
  }

  if (msg.role == "tool" || msg.role == "tool_result") {
    context::ToolPayload receipt;
    receipt.tool_call_id = msg.tool_call_id;
    receipt.tool_name = msg.tool_name;
    receipt.content = std::move(content);
    return receipt;
  }

  if (msg.role == "system") {
    context::SystemPayload system;
    system.content = std::move(content);
    return system;
  }

  context::UserPayload user;
  user.content = std::move(content);
  return user;
}

// Storage holds text that arrived from outside pu-cli, which may be in the
// producer's encoding: a localized library message, for instance, is written in
// the system locale. It is normalised here, the one way into storage, so that
// neither the request view nor the session file can inherit invalid bytes.
ChatMessage Normalized(const ChatMessage& msg) {
  if (text::IsValidUtf8(msg.content) && text::IsValidUtf8(msg.tool_name) &&
      text::IsValidUtf8(msg.reasoning_content) && text::IsValidUtf8(msg.tool_call_id)) {
    return msg;
  }
  ChatMessage clean = msg;
  clean.content = text::SanitizeUtf8(msg.content);
  clean.tool_name = text::SanitizeUtf8(msg.tool_name);
  clean.reasoning_content = text::SanitizeUtf8(msg.reasoning_content);
  clean.tool_call_id = text::SanitizeUtf8(msg.tool_call_id);
  return clean;
}

}  // namespace

void Transcript::Append(const ChatMessage& msg) {
  graph_.AppendAfterLeaf(ToPayload(Normalized(msg)), msg.timestamp);
}

std::vector<ChatMessage> Transcript::GetHistory() const {
  std::vector<ChatMessage> history;
  for (const context::MessageNode* node : graph_.Chain()) {
    history.push_back(session::RenderMessage(*node, static_cast<int>(history.size()) + 1));
  }
  return history;
}

size_t Transcript::Size() const { return graph_.Chain().size(); }

bool Transcript::HasPendingToolCalls() const {
  return graph_.LeafHasUnfinishedToolCalls();
}

boost::json::value Transcript::Serialize() const {
  boost::json::array arr;
  for (const ChatMessage& msg : GetHistory()) {
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
  if (!j.is_array()) return t;

  for (const boost::json::value& item : j.as_array()) {
    ChatMessage msg;
    msg.id = json::ValueOrDefault<int>(item, "id", 0);
    msg.timestamp = json::ValueOrDefault<std::string>(item, "timestamp", "");
    msg.role = json::ValueOrDefault<std::string>(item, "role", "");
    msg.content = json::ValueOrDefault<std::string>(item, "content", "");
    msg.tool_name = json::ValueOrDefault<std::string>(item, "tool_name", "");
    msg.tool_calls = json::ValueOrDefault<boost::json::value>(
        item, "tool_calls", boost::json::value(nullptr));
    msg.reasoning_content =
        json::ValueOrDefault<std::string>(item, "reasoning_content", "");
    msg.tool_call_id = json::ValueOrDefault<std::string>(item, "tool_call_id", "");
    t.Append(msg);
  }
  return t;
}

} // namespace pu
