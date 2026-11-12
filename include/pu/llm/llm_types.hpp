// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// What a provider is asked and what it answers with, as data.
//
// These types are the vocabulary a conversation is written in: the messages it holds, the tools
// it may call, and what a reply cost. They are separated from `LLMProvider` because both sides
// of a turn need them and only one side needs to know how a provider is reached — the session
// records what was said without caring which product said it, and the request it builds is the
// same request whichever provider carries it out.
//
// Nothing here names `LLMProvider`, so this header can be included by the layers that hold and
// format a conversation without pulling in the layer that talks to a network.
//

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "pu/core/base.hpp"
#include "pu/core/json.hpp"
#include "pu/core/tool_types.hpp"

namespace pu {

// How hard a provider is asked to think before answering. `kServerDefault` is the absence of a
// choice rather than a level, which is why it is not one of the four names a table may write.
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

// Who a message in a conversation is from. The four names are the wire's, not the session's:
// a provider reads them verbatim out of every request, so they are part of what is asked rather
// than of how a transcript is stored.
inline constexpr const char* kUserRole = "user";
inline constexpr const char* kAssistantRole = "assistant";
inline constexpr const char* kSystemRole = "system";
inline constexpr const char* kToolRole = "tool";

// A stretch of the model thinking aloud, kept with the signature that lets a provider verify it
// on a later turn rather than take the transcript's word for it.
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

  bool HasToolCalls() const {
    const boost::json::array* calls = tool_calls.if_array();
    return calls != nullptr && !calls->empty();
  }
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

}  // namespace pu
