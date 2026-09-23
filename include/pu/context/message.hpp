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

#include "pu/core/uuid.hpp"

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

// Every role payload carries text as parts, so a non-text part is an added
// variant alternative rather than a change to every consumer. Providers that
// accept only text flatten the parts.
struct TextPart {
  std::string text;
};

using ContentPart = std::variant<TextPart>;

inline std::string FlattenText(const std::vector<ContentPart>& parts) {
  std::string text;
  for (const ContentPart& part : parts) {
    text += std::get<TextPart>(part).text;
  }
  return text;
}

// Reasoning as received, kept whole so a provider that requires an echoed
// signature or an opaque block can be replayed exactly.
struct Reasoning {
  std::string provider;
  std::string signature;
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

struct UserPayload {
  std::vector<ContentPart> content;
};

struct AssistantPayload {
  std::vector<ContentPart> content;
  std::optional<Reasoning> reasoning;
  std::vector<ToolCallRecord> tool_calls;
};

struct SystemPayload {
  std::vector<ContentPart> content;
};

// The result of running a tool, answering the record with the same id.
struct ToolPayload {
  std::string tool_call_id;
  std::string tool_name;
  std::vector<ContentPart> content;
};

using MessagePayload =
    std::variant<UserPayload, AssistantPayload, SystemPayload, ToolPayload>;

struct MessageNode {
  MessageId id;
  std::string timestamp;
  MessagePayload payload;
  std::vector<MessageId> parents;
};

inline MessageNode MakeNode(MessagePayload payload,
                            std::vector<MessageId> parents = {}) {
  return MessageNode{NewMessageId(), std::string{}, std::move(payload),
                     std::move(parents)};
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
