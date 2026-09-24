// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The context model. Every stored turn is a node holding one of these payloads,
// and ChatMessage is the view a provider still requires.
//
// Not thread-safe. Caller must serialize access.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <boost/json.hpp>

#include "pu/core/json.hpp"
#include "pu/core/base.hpp"

namespace pu::context {

// Stable node identity. Independent of position, because the previous
// `id = size() + 1` scheme made rewind and branching unrepresentable.
using MessageId = std::string;

inline MessageId NewMessageId() { return uuid::Generate(); }

// The role words the payloads carry. The stored session and every provider
// round trip spell them this way, so they are named once here.
inline constexpr const char* kUserRole = "user";
inline constexpr const char* kAssistantRole = "assistant";
inline constexpr const char* kSystemRole = "system";
inline constexpr const char* kToolRole = "tool";

// Reasoning as received, kept in the provider's own encoding so it can be
// echoed back unchanged.
struct Reasoning {
  std::string raw_json;
};

enum class ToolCallStatus {
  kPending,
  kCompleted,
};

// The model's request to run a tool. Deliberately separate from the result it
// produced, which travels in its own ToolPayload node.
struct ToolCallRecord {
  std::string id;
  std::string name;
  // Kept as JSON rather than a string: providers disagree on whether arguments
  // travel as an object or an encoded string, and the projection decides that.
  boost::json::value arguments;
  ToolCallStatus status = ToolCallStatus::kPending;
};

// The shape a tool call travels in: the OpenAI function-call object, which the
// stored session, the providers, and the request path all agree on. Building it
// and reading it in one place is what keeps a call from changing shape between
// them.
inline boost::json::value ToolCallToJson(const ToolCallRecord& record) {
  return boost::json::value{
      {"id", record.id},
      {"type", "function"},
      {"function", {{"name", record.name}, {"arguments", record.arguments}}},
  };
}

// A call without a function object reads back as a record carrying only its id,
// so a caller can tell a malformed call from a complete one.
inline ToolCallRecord ToolCallFromJson(const boost::json::value& call) {
  ToolCallRecord record;
  record.id = json::ValueOrDefault<std::string>(call, "id", "");
  if (!json::HasKey(call, "function")) return record;

  const boost::json::value& function = call.at("function");
  record.name = json::ValueOrDefault<std::string>(function, "name", "");
  record.arguments =
      json::ValueOrDefault<boost::json::value>(function, "arguments", boost::json::object{});
  return record;
}

struct UserPayload {
  std::string content;
};

struct AssistantPayload {
  std::string content;
  std::optional<Reasoning> reasoning;
  std::vector<ToolCallRecord> tool_calls;
};

struct SystemPayload {
  std::string content;
};

// The result of running a tool, answering the record with the same id.
struct ToolPayload {
  std::string tool_call_id;
  std::string tool_name;
  std::string content;
};

using MessagePayload = std::variant<UserPayload, AssistantPayload, SystemPayload, ToolPayload>;

struct MessageNode {
  MessageId id;
  std::string timestamp;
  MessagePayload payload;
  // Empty for the first node. One parent rather than a list: a second one could
  // only describe two lines of reasoning converging, which is the choice the store
  // deliberately never keeps.
  MessageId parent{};
};

inline MessageNode MakeNode(MessagePayload payload) {
  return MessageNode{NewMessageId(), std::string{}, std::move(payload)};
}

// True while a tool call has not finished, which is what blocks switching the
// agent or backend mid-run.
inline bool HasUnfinishedToolCalls(const MessageNode& node) {
  const AssistantPayload* assistant = std::get_if<AssistantPayload>(&node.payload);
  if (assistant == nullptr) return false;
  for (const ToolCallRecord& call : assistant->tool_calls) {
    if (call.status != ToolCallStatus::kCompleted) return true;
  }
  return false;
}

}  // namespace pu::context
