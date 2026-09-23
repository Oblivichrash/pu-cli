// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>
#include <functional>

#include <boost/json.hpp>

#include "pu/core/cancel_token.hpp"

namespace pu {

// FROZEN: no new fields. Context features belong to MessageNode
// (include/pu/context/message.hpp); this struct is the compatibility view the
// request path renders from it.
struct ChatMessage {
  int id = 0;
  std::string timestamp;
  std::string role;
  std::string content;
  std::string tool_name;         // tool messages: name of the tool that produced the result
  boost::json::value tool_calls; // assistant messages: array of OpenAI-style tool calls
  std::string reasoning_content; // for DeepSeek thinking mode
  std::string tool_call_id;      // for tool messages: ID of the tool call

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
struct ChatResult {
  std::string content;
  std::string reasoning_content;
  std::vector<ToolCall> tool_calls;
};

class LLMProvider {
public:
  virtual ~LLMProvider() = default;

  // `content_callback` exists so a token reaches the user while the stream is
  // still open; everything the caller needs afterwards is in the result.
  virtual ChatResult Chat(
    const std::vector<ChatMessage>& history,
    const std::vector<ToolDefinition>& tools,
    std::function<void(const std::string&)> content_callback = nullptr,
    CancelToken cancel_token = nullptr
  ) = 0;

  virtual bool SupportsTools() const = 0;
  virtual std::string GetModelName() const = 0;
  virtual bool IsThinkingMode() const { return false; }
};

} // namespace pu
