// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/transcript.hpp"

#include "pu/core/json.hpp"

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

ChatMessage ToMessage(const context::MessageNode& node, int id) {
  ChatMessage msg;
  msg.id = id;
  msg.timestamp = node.timestamp;

  if (const auto* user = std::get_if<context::UserPayload>(&node.payload)) {
    msg.role = "user";
    msg.content = context::FlattenText(user->content);
  } else if (const auto* assistant =
                 std::get_if<context::AssistantPayload>(&node.payload)) {
    msg.role = "assistant";
    msg.content = context::FlattenText(assistant->content);
    if (assistant->reasoning) msg.reasoning_content = assistant->reasoning->raw_json;
    if (!assistant->tool_calls.empty()) {
      boost::json::array calls;
      for (const context::ToolCallRecord& record : assistant->tool_calls) {
        calls.push_back(boost::json::value{
            {"id", record.id},
            {"type", "function"},
            {"function", {{"name", record.name}, {"arguments", record.arguments}}},
        });
      }
      msg.tool_calls = std::move(calls);
    }
  } else if (const auto* system =
                 std::get_if<context::SystemPayload>(&node.payload)) {
    msg.role = "system";
    msg.content = context::FlattenText(system->content);
  } else {
    const context::ToolPayload& receipt = std::get<context::ToolPayload>(node.payload);
    msg.role = "tool";
    msg.content = context::FlattenText(receipt.content);
    msg.tool_name = receipt.tool_name;
    msg.tool_call_id = receipt.tool_call_id;
  }

  return msg;
}

}  // namespace

const context::MessageNode* Transcript::Find(const context::MessageId& id) const {
  const auto it = nodes_.find(id);
  return it == nodes_.end() ? nullptr : &it->second;
}

std::vector<const context::MessageNode*> Transcript::Chain() const {
  std::vector<const context::MessageNode*> chain;
  const context::MessageNode* node = Find(leaf_);
  while (node != nullptr) {
    chain.push_back(node);
    node = node->parents.empty() ? nullptr : Find(node->parents.front());
  }
  std::reverse(chain.begin(), chain.end());
  return chain;
}

void Transcript::MarkToolCallCompleted(const std::string& tool_call_id) {
  if (tool_call_id.empty()) return;

  context::MessageId id = leaf_;
  while (!id.empty()) {
    const auto it = nodes_.find(id);
    if (it == nodes_.end()) return;

    if (auto* assistant =
            std::get_if<context::AssistantPayload>(&it->second.payload)) {
      for (context::ToolCallRecord& record : assistant->tool_calls) {
        if (record.id == tool_call_id) {
          record.status = context::ToolCallStatus::kCompleted;
          return;
        }
      }
    }
    id = it->second.parents.empty() ? context::MessageId{}
                                    : it->second.parents.front();
  }
}

void Transcript::Append(const ChatMessage& msg) {
  context::MessagePayload payload = ToPayload(msg);

  std::string answered_call;
  if (const auto* receipt = std::get_if<context::ToolPayload>(&payload)) {
    answered_call = receipt->tool_call_id;
  }

  context::MessageNode node;
  node.id = context::NewMessageId();
  node.timestamp = msg.timestamp;
  node.payload = std::move(payload);
  if (!leaf_.empty()) node.parents.push_back(leaf_);

  leaf_ = node.id;
  nodes_.emplace(node.id, std::move(node));

  MarkToolCallCompleted(answered_call);
}

std::vector<ChatMessage> Transcript::GetHistory() const {
  std::vector<ChatMessage> history;
  for (const context::MessageNode* node : Chain()) {
    history.push_back(ToMessage(*node, static_cast<int>(history.size()) + 1));
  }
  return history;
}

size_t Transcript::Size() const { return Chain().size(); }

void Transcript::Compact(size_t keep_head, size_t keep_tail) {
  const std::vector<const context::MessageNode*> chain = Chain();
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
        const auto* receipt = std::get_if<context::ToolPayload>(&chain[j]->payload);
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
    summary.content = ToContent("[Compressed: " + std::to_string(tail_start - keep_head) +
                                " messages omitted]");
    summary.is_synthetic = true;

    context::MessageNode node;
    node.id = context::NewMessageId();
    node.payload = std::move(summary);
    if (!kept.empty()) node.parents.push_back(kept.back());

    kept.push_back(node.id);
    nodes_.emplace(node.id, std::move(node));
  }

  if (tail_start < chain.size()) {
    // The first kept tail node pointed at a node that is being dropped.
    nodes_[chain[tail_start]->id].parents = {kept.back()};
    for (size_t i = tail_start; i < chain.size(); ++i) kept.push_back(chain[i]->id);
  }

  const std::set<context::MessageId> keeping(kept.begin(), kept.end());
  for (auto it = nodes_.begin(); it != nodes_.end();) {
    if (keeping.count(it->first) != 0) {
      ++it;
    } else {
      it = nodes_.erase(it);
    }
  }

  leaf_ = kept.empty() ? context::MessageId{} : kept.back();
}

bool Transcript::HasPendingToolCalls() const {
  const context::MessageNode* last = Find(leaf_);
  return last != nullptr && context::HasUnfinishedToolCalls(*last);
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
