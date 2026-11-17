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
#include "pu/llm/llm_types.hpp"
#include "pu/paging/origin.hpp"

namespace pu::session {

using MessageId = std::string;

inline MessageId NewMessageId() { return uuid::Generate(); }

// The role names a message carries are the wire's vocabulary and live in `llm_types`, so a
// provider can read them without naming a session. They are in `pu`, which this namespace is
// nested in, so a transcript header sees them without a using-declaration.

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

// What a call's arguments are replaced with once they are too large to re-send every turn.
//
// The argument body is the last pool with no bound. A `write_file` of a thousand lines puts
// ten thousand tokens on the *input* side of every later request, forever, while the result
// of that call -- which is usually a one-line confirmation -- was already elided on the output
// side. Nothing else in a transcript grows like this, because everything else is either
// compressed by a page or removed with its pair.
//
// `bytes` and `lines` are what say how much went, `path` and `stored_in` are how a reader gets
// it back. The argument keys are kept by name so that a model can still tell a `write_file`
// from a `bash`, and `elided` is the marker `TombstonedReceipt` already uses for the same
// reason: a field that says what happened reads better than a body that is simply missing.
struct ElidedArguments {
  std::size_t bytes = 0;
  std::size_t lines = 0;
  // The argument that carried the text, when one key held nearly all of it: `content` for a
  // `write_file`, `command` for a shell call. Empty when the body was spread across keys.
  std::string path;
  // Every other key, as it was, so the call still reads as the call it was.
  boost::json::object fields{};
};

inline boost::json::value ElidedArgumentsAsJson(const ElidedArguments& elided) {
  boost::json::object out = elided.fields;
  out["elided"] = "arguments";
  out["bytes"] = static_cast<std::uint64_t>(elided.bytes);
  out["lines"] = static_cast<std::uint64_t>(elided.lines);
  if (!elided.path.empty()) out["omitted"] = elided.path;
  return out;
}

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
  // The call's arguments, in full. Kept because a record is also the thing a person reads back
  // and a transcript is saved and reloaded: an elision is a decision about *one request*, not
  // about what happened, and a graph that stored the elided form would have thrown the
  // arguments away for every later turn too.
  boost::json::value arguments;
  ToolCallStatus status = ToolCallStatus::kPending;
  // Set once the projection has replaced `arguments` with a stand-in, so the remaining bytes
  // and the way back are both on the record. Absent on the overwhelming majority of calls:
  // only an argument body large enough to matter is ever elided, and a call that was elided is
  // elided by identity rather than re-measured every turn.
  //
  // Last in the struct on purpose: the four fields above are what a call site names, and a
  // member inserted between them would silently move every positional initialiser along.
  std::optional<ElidedArguments> elided{};
};

inline boost::json::value ToolCallToJson(const ToolCallRecord& record) {
  boost::json::value arguments = record.arguments;
  // The elision is applied here rather than to `arguments` itself: this is the function that
  // builds what the provider sees, and the record keeps the text the provider would have seen.
  if (record.elided.has_value()) arguments = ElidedArgumentsAsJson(*record.elided);
  return boost::json::value{
      {"id", record.id},
      {"type", "function"},
      {"function", {{"name", record.name}, {"arguments", std::move(arguments)}}},
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
  // Where the result came from, when it is a page: the question, the run, and how late the
  // run was. Read out of `content` once, as the result enters the conversation, because the
  // text does not change afterwards — a projection that parsed it again every turn would be
  // asking a question whose answer it already had.
  std::optional<paging::Origin> origin{};
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

inline constexpr const char* kCancelledReceipt =
    "The user cancelled the turn before this call ran, so it produced no result. Wait for the "
    "user's next message before deciding whether this call is still worth making";

}  // namespace pu::session
