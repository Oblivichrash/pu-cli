// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>
#include <functional>

#include <boost/json.hpp>

#include "pu/core/cancel_token.hpp"

namespace pu {

struct ChatMessage {
  int id = 0;
  std::string timestamp;
  std::string role;
  std::string content;
  std::string tool_name;         // tool messages: name of the tool that produced the result
  std::string tool_calls_json;   // Serialized tool_calls for assistant messages
  std::string reasoning_content; // for DeepSeek thinking mode
  std::string tool_call_id;      // for tool messages: ID of the tool call
};

struct ToolDefinition {
  std::string name;
  std::string description;
  std::string parameters_schema;
};

struct ToolCall {
  std::string id;
  std::string name;
  boost::json::value arguments;
};

struct ChatResult {
  std::string content;
  std::vector<ToolCall> tool_calls;
  int input_tokens = 0;
  int output_tokens = 0;
  std::string reasoning_content;
};

class LLMProvider {
public:
  virtual ~LLMProvider() = default;

  virtual ChatResult Chat(
    const std::vector<ChatMessage>& history,
    const std::vector<ToolDefinition>& tools,
    std::function<void(const std::string&)> content_callback = nullptr,
    std::function<void(const ToolCall&)> tool_callback = nullptr,
    CancelToken cancel_token = nullptr
  ) = 0;

  virtual bool SupportsTools() const = 0;
  virtual std::string GetModelName() const = 0;
  virtual bool IsThinkingMode() const { return false; }
};

} // namespace pu
