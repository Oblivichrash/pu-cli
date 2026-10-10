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

namespace pu::context {

using MessageId = std::string;

inline MessageId NewMessageId() { return uuid::Generate(); }

inline constexpr const char* kUserRole = "user";
inline constexpr const char* kAssistantRole = "assistant";
inline constexpr const char* kSystemRole = "system";
inline constexpr const char* kToolRole = "tool";

struct Reasoning {
  std::string raw_json;
};

enum class ToolCallStatus {
  kPending,
  kCompleted,
};

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
};

struct AssistantPayload {
  std::string content;
  std::optional<Reasoning> reasoning;
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
    if (call.status != ToolCallStatus::kCompleted) return true;
  }
  return false;
}

}  // namespace pu::context
