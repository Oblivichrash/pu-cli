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

struct TokenUsage {
  int prompt_tokens = 0;
  int completion_tokens = 0;
};

struct ChatResult {
  std::string content;
  std::string reasoning_content;
  std::vector<ToolCall> tool_calls;
  std::optional<TokenUsage> usage;
  std::string finish_reason;
  std::string model;
};

class LLMProvider {
 public:
  virtual ~LLMProvider() = default;

  virtual ChatResult Chat(const std::vector<ChatMessage>& history,
                          const std::vector<ToolDefinition>& tools,
                          std::function<void(const std::string&)> content_callback = nullptr,
                          CancelToken cancel_token = nullptr,
                          std::function<void(const std::string&)> reasoning_callback = nullptr) = 0;

  virtual bool SupportsTools() const = 0;
};

}  // namespace pu
