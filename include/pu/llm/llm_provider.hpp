// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>
#include "pu/conversation.hpp"

namespace pu {

// ToolDefinition used across provider interfaces
struct ToolDefinition {
  std::string name;
  std::string description;
  std::string parameters_schema;  // JSON schema as string
};

struct ToolCall {
  std::string name;
  nlohmann::json arguments;  // unified as object
};

struct ChatResult {
  std::string content;
  std::vector<ToolCall> tool_calls;
  int input_tokens = 0;
  int output_tokens = 0;
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
  virtual bool SupportsStrictMode() const = 0;
  virtual std::string GetModelName() const = 0;
};

} // namespace pu
