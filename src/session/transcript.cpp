// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/transcript.hpp"

#include "pu/core/json.hpp"
#include "pu/session/request.hpp"

#include <algorithm>
#include <set>

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

}  // namespace

void Transcript::Append(const ChatMessage& msg) {
  graph_.AppendAfterLeaf(ToPayload(msg), msg.timestamp);
}

std::vector<ChatMessage> Transcript::GetHistory() const {
  std::vector<ChatMessage> history;
  for (const context::MessageNode* node : graph_.Chain()) {
    history.push_back(session::RenderMessage(*node, static_cast<int>(history.size()) + 1));
  }
  return history;
}

size_t Transcript::Size() const { return graph_.Chain().size(); }

void Transcript::Compact(size_t keep_head, size_t keep_tail) {
  const std::vector<const context::MessageNode*> chain = graph_.Chain();
  if (chain.size() <= keep_head + keep_tail) return;

  size_t tail_start = chain.size() - keep_tail;
  for (size_t i = std::min(tail_start, chain.size() - 1); i > keep_head; --i) {
    const auto* assistant =
        std::get_if<context::AssistantPayload>(&chain[i]->payload);
    if (assistant == nullptr || assistant->tool_calls.empty()) continue;

    // Keep every tool call together with the tool result it produced.
    bool all_matched = true;
    for (const context::ToolCallRecord& record : assistant->tool_calls) {
      if (record.id.empty()) continue;
      bool matched = false;
      for (size_t j = i + 1; j < chain.size(); ++j) {
        const auto* receipt =
            std::get_if<context::ToolPayload>(&chain[j]->payload);
        if (receipt != nullptr && receipt->tool_call_id == record.id) {
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

  std::vector<context::MessageId> kept;
  kept.reserve(chain.size() + 1);
  for (size_t i = 0; i < keep_head; ++i) kept.push_back(chain[i]->id);

  if (tail_start > keep_head) {
    context::SystemPayload summary;
    summary.content = ToContent("[Compressed: " +
                                std::to_string(tail_start - keep_head) +
                                " messages omitted]");
    summary.is_synthetic = true;

    context::MessageNode node = context::MakeNode(std::move(summary));
    const context::MessageId summary_id = node.id;
    graph_.Add(std::move(node));
    if (!kept.empty()) graph_.SetParents(summary_id, {kept.back()});
    kept.push_back(summary_id);
  }

  if (tail_start < chain.size()) {
    // The first kept tail node pointed at a node that is being dropped.
    graph_.SetParents(chain[tail_start]->id, {kept.back()});
    for (size_t i = tail_start; i < chain.size(); ++i) kept.push_back(chain[i]->id);
  }

  graph_.RetainOnly(kept);
  graph_.SetLeaf(kept.empty() ? context::MessageId{} : kept.back());
}

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
