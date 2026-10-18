// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>
#include <span>
#include <functional>
#include <optional>

#include <boost/json.hpp>

#include "pu/core/base.hpp"
#include "pu/core/json.hpp"

namespace pu {

enum class ThinkingLevel {
  kServerDefault,
  kNone,
  kLow,
  kMedium,
  kHigh,
};

inline const char* ThinkingLevelName(ThinkingLevel level) {
  switch (level) {
    case ThinkingLevel::kNone:
      return "none";
    case ThinkingLevel::kLow:
      return "low";
    case ThinkingLevel::kMedium:
      return "medium";
    case ThinkingLevel::kHigh:
      return "high";
    case ThinkingLevel::kServerDefault:
      break;
  }
  return "default";
}

inline ThinkingLevel ParseThinkingLevel(const std::string& name) {
  if (name == "none") return ThinkingLevel::kNone;
  if (name == "low") return ThinkingLevel::kLow;
  if (name == "medium") return ThinkingLevel::kMedium;
  if (name == "high") return ThinkingLevel::kHigh;
  return ThinkingLevel::kServerDefault;
}

inline ThinkingLevel ReadThinkingLevel(const boost::json::value& j) {
  if (json::HasKey(j, "thinking") && j.at("thinking").is_string()) {
    return ParseThinkingLevel(boost::json::value_to<std::string>(j.at("thinking")));
  }
  return ThinkingLevel::kServerDefault;
}

struct ReasoningBlock {
  std::string text;
  std::string signature;   // Anthropic thinking blocks: must be returned verbatim
  boost::json::value raw;  // the provider's own encoding, replayed when it round-trips
};

struct ChatMessage {
  int id = 0;
  std::string timestamp;
  std::string role;
  std::string content;
  std::string tool_name;          // tool messages: name of the tool that produced the result
  boost::json::value tool_calls;  // assistant messages: array of OpenAI-style tool calls
  std::vector<ReasoningBlock> reasoning;
  std::string tool_call_id;       // for tool messages: ID of the tool call

  bool HasToolCalls() const {
    const boost::json::array* calls = tool_calls.if_array();
    return calls != nullptr && !calls->empty();
  }
};

struct ToolDefinition {
  std::string name;
  std::string description;
  boost::json::value parameters;

  const boost::json::value& Parameters() const {
    static const boost::json::value kEmpty = boost::json::object{};
    return parameters.is_object() ? parameters : kEmpty;
  }
};

struct ToolCall {
  std::string id;
  std::string name;
  boost::json::value arguments;
};

// What a provider counted for a turn. A field a backend does not report stays zero rather
// than being derived: `total_tokens` is the provider's own total, and a sum of the two
// counts beside it would read as a figure the provider gave.
// What one call cost, in the product's own unit. The number is the one the gateway sent, and
// the unit is the one the gateway table names: no conversion is done, because no rate exists
// that would make two products' units comparable.
struct Cost {
  double amount = 0.0;
  std::string unit;
};

struct TokenUsage {
  int prompt_tokens = 0;
  int completion_tokens = 0;
  int total_tokens = 0;
  int cached_tokens = 0;  // the prompt the provider answered from its cache
  // Absent where the gateway prices nothing — which is not the same as a call it priced at
  // zero. One gateway's `credit` read 0 for a one-token answer and 0.17 for a 203-token one;
  // another sends no such field at all, and only the difference between those two can be
  // summed into a total that means anything.
  std::optional<Cost> cost;
};

using TokenCallback = std::function<void(std::string_view)>;

struct ChatResult {
  std::string content;
  std::vector<ReasoningBlock> reasoning;
  std::vector<ToolCall> tool_calls;
  std::optional<TokenUsage> usage;
  // The usage object exactly as the gateway sent it, so a field nothing reads yet is
  // carried rather than dropped. Null where the backend reports no usage object.
  boost::json::value raw_usage;
  // Keys a frame carried that no code names. A gateway is free to add one, so this is a
  // signal rather than an error: it is what says a field the backend should have read
  // went unnoticed, and it is asserted by the tests that pin a captured stream.
  std::vector<std::string> unread_fields;
  std::vector<std::string> seen_fields;
  // Shape fields the reply never carried: a name this build reads that the gateway no
  // longer sends.
  std::vector<std::string> missing_fields;
  std::string finish_reason;
  std::string model;
};

struct ChatRequest {
  std::span<const ChatMessage> history;
  std::span<const ToolDefinition> tools;
  TokenCallback on_content;
  TokenCallback on_reasoning;
  std::function<void(const ToolCall&)> on_tool_call;
  CancelToken cancel_token;

  ChatRequest() = default;

  ChatRequest(std::span<const ChatMessage> history_messages,
              std::span<const ToolDefinition> tool_definitions)
      : history(history_messages), tools(tool_definitions) {}

  ChatRequest(std::span<const ChatMessage> history_messages,
              std::span<const ToolDefinition> tool_definitions, TokenCallback content)
      : history(history_messages), tools(tool_definitions), on_content(std::move(content)) {}
};

class LLMProvider {
 public:
  virtual ~LLMProvider() = default;

  virtual ChatResult Chat(const ChatRequest& request) = 0;
};

}  // namespace pu
