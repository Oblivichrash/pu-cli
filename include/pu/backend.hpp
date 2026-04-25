// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// Abstract backend interface for language model providers.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pu::backend {

// ============================================================================
// Token types emitted during streaming
// ============================================================================
enum class TokenType {
  kReasoning,   // Model's internal reasoning (e.g., DeepSeek-R1)
  kContent      // Final answer content
};

// ============================================================================
// Streaming callback signature
// ============================================================================
using ChatCallback = std::function<void(TokenType type,
                                        std::string_view token,
                                        bool is_final)>;

// ============================================================================
// Tool-related types
// ============================================================================
struct ToolParameterSchema {
  std::string raw_schema;   // JSON Schema string (e.g. {"type":"object", ...})
};

struct ToolDefinition {
  std::string name;
  std::string description;
  ToolParameterSchema parameters;
};

struct ToolCall {
  std::string id;          // optional, may be empty
  std::string name;        // function name
  std::string arguments;   // JSON string of arguments
};

// ============================================================================
// Message structure representing a single conversation turn
// ============================================================================
struct Message {
  enum class Role { kSystem, kUser, kAssistant, kTool };
  Role role;
  std::string content;
  std::string tool_name;              // only used when role == kTool
  std::vector<ToolCall> tool_calls;   // only used when role == kAssistant
};

// ============================================================================
// Tool callback type
// ============================================================================
using ToolCallback = std::function<void(const ToolCall& call)>;

// ============================================================================
// Abstract backend interface
// ============================================================================
class Backend {
 public:
  struct Config {
    std::string model;
    float temperature = 0.7f;
    std::optional<std::string> system_prompt;
  };

  explicit Backend(Config config) : config_(std::move(config)) {}
  virtual ~Backend() = default;

  // Non-copyable, movable
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
  Backend(Backend&&) noexcept = default;
  Backend& operator=(Backend&&) noexcept = default;

  /// Send conversation history and receive streaming tokens via callback.
  virtual void Chat(const std::vector<Message>& history,
                    ChatCallback cb) = 0;

  /// Send conversation history with tool definitions. content_cb receives
  /// content tokens and reasoning tokens, and is_final signals end of stream.
  /// tool_cb receives each tool call requested by the model.
  virtual void Chat(const std::vector<Message>& history,
                    const std::vector<ToolDefinition>& tools,
                    ChatCallback content_cb,
                    ToolCallback tool_cb) = 0;

 protected:
  Config config_;
};

}  // namespace pu::backend
