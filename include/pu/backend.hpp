// SPDX-License-Identifier: GPL-3.0-only
//
// Abstract backend interface for language model providers.

#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pu::backend {

enum class TokenType {
  kReasoning,
  kContent
};

using ChatCallback = std::function<void(TokenType type,
                                        std::string_view token,
                                        bool is_final)>;

struct ToolParameterSchema {
  std::string raw_schema;
};

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
  Message(Role role, std::string content)
      : role(role), content(std::move(content)) {}
  Message(Role role, std::string tool_name, std::string content)
      : role(role), content(std::move(content)), tool_name(std::move(tool_name)) {}
  Message(Role role, std::vector<ToolCall> tool_calls)
      : role(role), tool_calls(std::move(tool_calls)) {}
};

using ToolCallback = std::function<void(const ToolCall& call)>;

class Backend {
 public:
  struct Config {
    std::string model;
    float temperature = 0.7f;
    std::optional<std::string> system_prompt;
  };

  explicit Backend(Config config) : config_(std::move(config)) {}
  virtual ~Backend() = default;

  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
  Backend(Backend&&) noexcept = default;
  Backend& operator=(Backend&&) noexcept = default;

  virtual void Chat(const std::vector<Message>& history,
                    ChatCallback cb) = 0;

  virtual void Chat(const std::vector<Message>& history,
                    const std::vector<ToolDefinition>& tools,
                    ChatCallback content_cb,
                    ToolCallback tool_cb) = 0;

  virtual bool SupportsTools() const { return false; }

 protected:
  Config config_;

  // Build a message list with the system prompt injected once, if configured and not already present.
  std::vector<Message> BuildMessagesWithSystemPrompt(const std::vector<Message>& history) const {
    std::vector<Message> messages;
    if (config_.system_prompt &&
        std::none_of(history.begin(), history.end(),
                     [](const auto& m) { return m.role == Message::Role::kSystem; })) {
      messages.push_back({Message::Role::kSystem, *config_.system_prompt});
    }
    for (const auto& msg : history) {
      messages.push_back(msg);
    }
    return messages;
  }
};

}  // namespace pu::backend
