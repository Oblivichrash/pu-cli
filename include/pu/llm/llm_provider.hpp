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

// How much reasoning a backend should spend on an answer. The words are the ones
// OpenAI uses; `kServerDefault` is the absent setting, which is what "on" meant
// before there was a level, and `kNone` is the switch it replaced.
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

// Reads the field as a level, accepting the boolean it replaced so that a file
// written before it keeps meaning what it meant: `false` was the only level that
// switch could name, and `true` was the absent setting.
inline ThinkingLevel ReadThinkingLevel(const boost::json::value& j) {
  if (json::HasKey(j, "thinking") && j.at("thinking").is_string()) {
    return ParseThinkingLevel(boost::json::value_to<std::string>(j.at("thinking")));
  }
  if (json::HasKey(j, "enable_thinking") && j.at("enable_thinking").is_bool()) {
    return boost::json::value_to<bool>(j.at("enable_thinking")) ? ThinkingLevel::kServerDefault
                                                                : ThinkingLevel::kNone;
  }
  return ThinkingLevel::kServerDefault;
}

// FROZEN: a compatibility view rendered from MessageNode; see ARCHITECTURE.md, Data Flow.
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

// What one request produced. Tool calls belong here because the caller acts on
// them after the stream has ended, the same way it acts on the text.
// What one request cost, as the provider counted it. Absent means the provider
// reported nothing, which is not the same as a measured zero: a caller that
// budgets tokens has to tell those apart.
struct TokenUsage {
  int prompt_tokens = 0;
  int completion_tokens = 0;
};

struct ChatResult {
  std::string content;
  std::string reasoning_content;
  std::vector<ToolCall> tool_calls;
  std::optional<TokenUsage> usage;
  // Why the provider stopped, in the provider's own word: OpenAI calls it
  // `finish_reason`, Ollama calls it `done_reason`, and the vocabularies overlap
  // without being the same. Kept verbatim so a caller can tell a reply the model
  // chose to end from one that ran into the token limit or the content filter.
  std::string finish_reason;
  // The model that answered, as the response names it. A gateway may serve
  // something other than what was asked for, and a tag may resolve to a dated
  // build, so this is the only place the request can learn who replied. Empty
  // when the response named nothing, which is not the same as the requested name.
  std::string model;
};

class LLMProvider {
 public:
  virtual ~LLMProvider() = default;

  // `content_callback` exists so a token reaches the user while the stream is
  // still open; everything the caller needs afterwards is in the result. Reasoning
  // has a sink of its own because it arrives on its own channel and is shown beside
  // the answer rather than in it.
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
