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
  std::string signature;
  boost::json::value raw;
};

struct ChatMessage {
  int id = 0;
  std::string timestamp;
  std::string role;
  std::string content;
  std::string tool_name;
  boost::json::value tool_calls;
  std::vector<ReasoningBlock> reasoning;
  std::string tool_call_id;
  // The peer a user turn arrived from, drawn where the page would say "You". Never part of
  // the request; see docs/ARCHITECTURE.md#peer-messaging.
  std::string from_peer;

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

struct Cost {
  double amount = 0.0;
  std::string unit;
};

// A field a backend does not report stays zero rather than being derived. docs/providers.md#usage
struct TokenUsage {
  int prompt_tokens = 0;
  int completion_tokens = 0;
  int total_tokens = 0;
  int cached_tokens = 0;
  std::optional<Cost> cost;
};

using TokenCallback = std::function<void(std::string_view)>;

struct ChatResult {
  std::string content;
  std::vector<ReasoningBlock> reasoning;
  std::vector<ToolCall> tool_calls;
  std::optional<TokenUsage> usage;
  boost::json::value raw_usage;
  std::vector<std::string> unread_fields;
  std::vector<std::string> seen_fields;
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
