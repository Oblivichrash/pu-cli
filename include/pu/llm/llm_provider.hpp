// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>
#include "pu/conversation.hpp"

namespace pu {

struct ToolDefinition {
  std::string name;
  std::string description;
  std::string parameters_schema;
};

struct ToolCall {
  std::string id;  // unique identifier for the tool call
  std::string name;
  nlohmann::json arguments;
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
    std::function<void(const ToolCall&)> tool_callback = nullptr
  ) = 0;

  virtual bool SupportsTools() const = 0;
  virtual std::string GetModelName() const = 0;
};

} // namespace pu
