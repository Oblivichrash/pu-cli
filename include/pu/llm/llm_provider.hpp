// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>
#include <functional>
#include <optional>

#include <boost/json.hpp>

#include "pu/core/base.hpp"
#include "pu/core/json.hpp"

namespace pu {

// How much reasoning a backend should spend. The words are OpenAI's; `kServerDefault` is the
// absent setting and `kNone` the switch it replaced.
enum class ThinkingLevel {
  kServerDefault,
  kNone,
  kLow,
  kMedium,
  kHigh,
};

// The word a level travels as, in the configuration file and on the wire.
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

// An unrecognised word reads as the absent setting, the way an unrecognised stored
// value reads as the one the store defaults to.
inline ThinkingLevel ParseThinkingLevel(const std::string& name) {
  if (name == "none") return ThinkingLevel::kNone;
  if (name == "low") return ThinkingLevel::kLow;
  if (name == "medium") return ThinkingLevel::kMedium;
  if (name == "high") return ThinkingLevel::kHigh;
  return ThinkingLevel::kServerDefault;
}

// Anything that is not one of the level names — an absent field, a foreign word, a value of
// the wrong type — reads as the absent setting.
inline ThinkingLevel ReadThinkingLevel(const boost::json::value& j) {
  if (json::HasKey(j, "thinking") && j.at("thinking").is_string()) {
    return ParseThinkingLevel(boost::json::value_to<std::string>(j.at("thinking")));
  }
  return ThinkingLevel::kServerDefault;
}

// A compatibility view rendered from MessageNode.
struct ChatMessage {
  int id = 0;
  std::string timestamp;
  std::string role;
  std::string content;
  std::string tool_name;          // tool messages: name of the tool that produced the result
  boost::json::value tool_calls;  // assistant messages: array of OpenAI-style tool calls
  std::string reasoning_content;  // for DeepSeek thinking mode
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

  // Providers embed the schema directly in their request payload, so an unset
  // or non-object schema is reported as an empty object rather than `null`.
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

// What one request cost, as the provider counted it. Absent means it reported nothing,
// which is not a measured zero: a caller that budgets tokens has to tell those apart.
struct TokenUsage {
  int prompt_tokens = 0;
  int completion_tokens = 0;
};

// Tool calls belong here because the caller acts on them after the stream has ended.
struct ChatResult {
  std::string content;
  std::string reasoning_content;
  std::vector<ToolCall> tool_calls;
  std::optional<TokenUsage> usage;
  // The provider's own word (OpenAI's `finish_reason`, Ollama's `done_reason`), kept
  // verbatim so the caller can tell a chosen end from a limit or a filter.
  std::string finish_reason;
  // The model that answered, as the response names it: the only place the request can
  // learn who replied. Empty when the response named nothing.
  std::string model;
};

class LLMProvider {
 public:
  virtual ~LLMProvider() = default;

  // `content_callback` lets a token reach the user while the stream is open; reasoning
  // has a sink of its own because it is shown beside the answer, not in it.
  virtual ChatResult Chat(const std::vector<ChatMessage>& history,
                          const std::vector<ToolDefinition>& tools,
                          std::function<void(const std::string&)> content_callback = nullptr,
                          CancelToken cancel_token = nullptr,
                          std::function<void(const std::string&)> reasoning_callback = nullptr) = 0;

  virtual bool SupportsTools() const = 0;
  // Whether a thinking level reaches this backend at all, so a caller offers the
  // setting only where it lands.
  virtual bool SupportsThinkingLevel() const { return false; }
};

}  // namespace pu
