// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pu::backend {

enum class TokenType { kReasoning, kContent };

using ChatCallback = std::function<void(TokenType, std::string_view, bool)>;

struct ToolParameterSchema { std::string raw_schema; };

struct ToolDefinition {
  std::string name;
  std::string description;
  ToolParameterSchema parameters;
};

struct ToolCall {
  std::string id;
  std::string name;
  std::string arguments;
};

struct Message {
  enum class Role { kSystem, kUser, kAssistant, kTool };
  Role role;
  std::string content;
  std::string tool_name;
  std::vector<ToolCall> tool_calls;

  Message() = default;
  Message(Role r, std::string c) : role(r), content(std::move(c)) {}
  Message(Role r, std::string tn, std::string c)
      : role(r), content(std::move(c)), tool_name(std::move(tn)) {}
  Message(Role r, std::vector<ToolCall> tc)
      : role(r), tool_calls(std::move(tc)) {}
};

using ToolCallback = std::function<void(const ToolCall&)>;

class Backend {
 public:
  struct Config {
    std::string model;
    float temperature = 0.7f;
    std::optional<std::string> system_prompt;
  };

  explicit Backend(Config c) : config_(std::move(c)) {}
  virtual ~Backend() = default;

  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
  Backend(Backend&&) noexcept = default;
  Backend& operator=(Backend&&) noexcept = default;

  virtual void Chat(const std::vector<Message>& history,
                    ChatCallback cb) = 0;
  virtual void Chat(const std::vector<Message>& history,
                    const std::vector<ToolDefinition>& tools,
                    ChatCallback content_cb, ToolCallback tool_cb) = 0;
  virtual bool SupportsTools() const { return false; }

 protected:
  Config config_;
};

}  // namespace pu::backend
