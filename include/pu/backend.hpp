// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// Abstract backend interface for language model providers.

#ifndef PU_BACKEND_HPP
#define PU_BACKEND_HPP

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
// Message structure representing a single conversation turn
// ============================================================================
struct Message {
  enum class Role { kSystem, kUser, kAssistant };
  Role role;
  std::string content;
};

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

  explicit Backend(Config config);
  virtual ~Backend() = default;

  // Non-copyable, movable
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
  Backend(Backend&&) = default;
  Backend& operator=(Backend&&) = default;

  /// Send conversation history and receive streaming tokens via callback.
  /// @param history The full conversation context.
  /// @param cb Callback invoked for each token and at stream end (is_final=true).
  /// @throws std::runtime_error on network, HTTP, or parsing failures.
  virtual void Chat(const std::vector<Message>& history,
                    ChatCallback cb) = 0;
};

}  // namespace pu::backend

#endif  // PU_BACKEND_HPP
