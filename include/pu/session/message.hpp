// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <boost/json.hpp>

#include "pu/core/json.hpp"
#include "pu/core/base.hpp"

namespace pu::session {

using MessageId = std::string;

inline MessageId NewMessageId() { return uuid::Generate(); }

inline constexpr const char* kUserRole = "user";
inline constexpr const char* kAssistantRole = "assistant";
inline constexpr const char* kSystemRole = "system";
inline constexpr const char* kToolRole = "tool";

struct Reasoning {
  std::string text;
  std::string signature;
  boost::json::value raw;
};

inline boost::json::value ReasoningToJson(const Reasoning& reasoning) {
  boost::json::object out = {{"text", reasoning.text}};
  if (!reasoning.signature.empty()) out["signature"] = reasoning.signature;
  if (!reasoning.raw.is_null()) out["raw"] = reasoning.raw;
  return out;
}

inline Reasoning ReasoningFromJson(const boost::json::value& value) {
  Reasoning reasoning;
  if (value.is_string()) {
    reasoning.text = boost::json::value_to<std::string>(value);
    return reasoning;
  }
  reasoning.text = json::ValueOrDefault<std::string>(value, "text", "");
  reasoning.signature = json::ValueOrDefault<std::string>(value, "signature", "");
  if (json::HasKey(value, "raw")) reasoning.raw = value.at("raw");
  return reasoning;
}

enum class ToolCallStatus {
  kPending,
  kCompleted,
  kInterrupted,
};

inline constexpr const char* kPendingStatusName = "pending";
inline constexpr const char* kCompletedStatusName = "completed";
inline constexpr const char* kInterruptedStatusName = "interrupted";

inline std::string ToolCallStatusName(ToolCallStatus status) {
  switch (status) {
    case ToolCallStatus::kPending:
      return kPendingStatusName;
    case ToolCallStatus::kCompleted:
      return kCompletedStatusName;
    case ToolCallStatus::kInterrupted:
      return kInterruptedStatusName;
  }
  return kPendingStatusName;
}

struct ToolCallRecord {
  std::string id;
  std::string name;
  boost::json::value arguments;
  ToolCallStatus status = ToolCallStatus::kPending;
};

inline boost::json::value ToolCallToJson(const ToolCallRecord& record) {
  return boost::json::value{
      {"id", record.id},
      {"type", "function"},
      {"function", {{"name", record.name}, {"arguments", record.arguments}}},
  };
}

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
  // The peer whose message this is, empty for the person at the page. It is a fact about the
  // turn rather than about its text: the model never reads it, and the page uses it where it
  // would otherwise say "You".
  std::string from_peer;
};

struct AssistantPayload {
  std::string content;
  std::vector<Reasoning> reasoning;
  std::vector<ToolCallRecord> tool_calls;
};

struct SystemPayload {
  std::string content;
};

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
  MessageId parent{};
};

inline MessageNode MakeNode(MessagePayload payload) {
  return MessageNode{NewMessageId(), std::string{}, std::move(payload)};
}

inline bool HasUnfinishedToolCalls(const MessageNode& node) {
  const AssistantPayload* assistant = std::get_if<AssistantPayload>(&node.payload);
  if (assistant == nullptr) return false;
  for (const ToolCallRecord& call : assistant->tool_calls) {
    if (call.status == ToolCallStatus::kPending) return true;
  }
  return false;
}

inline bool HasToolCallsInterrupted(const MessageNode& node) {
  const AssistantPayload* assistant = std::get_if<AssistantPayload>(&node.payload);
  if (assistant == nullptr) return false;
  for (const ToolCallRecord& call : assistant->tool_calls) {
    if (call.status == ToolCallStatus::kInterrupted) return true;
  }
  return false;
}

inline constexpr const char* kCancelledReceipt =
    "The user cancelled the turn before this call ran, so it produced no result. Wait for the "
    "user's next message before deciding whether this call is still worth making";

inline constexpr const char* kPeerOriginNote =
    "This turn arrived from another agent. If the message needs an answer, send it straight back "
    "to the agent that asked, then stop. Do not relay the request onward to anyone else, and do "
    "not open with a tool call when you can answer in words";

inline constexpr const char* kCommandStoppedReceipt =
    "The user stopped this command while it was running. The turn is still going, so the rest of "
    "your plan is unaffected. Do not repeat this command unchanged; either adjust it or continue "
    "with something else";

}  // namespace pu::session
