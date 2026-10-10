// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The shape every HTTP streaming backend shares. What differs between them — the path,
// the line format, the headers — is a hook here, so the request/stream/collect pipeline
// exists once rather than per provider.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "pu/core/http_client.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu {

class StreamingProvider : public LLMProvider {
 public:
  ~StreamingProvider() override = default;

  // Not overridable: a provider differs in how it reads a line, never in how the request
  // is sent or the answer collected.
  ChatResult Chat(const std::vector<ChatMessage>& history,
                  const std::vector<ToolDefinition>& tools,
                  std::function<void(const std::string&)> content_callback = nullptr,
                  CancelToken cancel_token = nullptr,
                  std::function<void(const std::string&)> reasoning_callback = nullptr) final;

  bool SupportsTools() const override { return true; }

 protected:
  StreamingProvider(std::string host, std::string api_key,
                    std::unique_ptr<pu::http::HttpClient> http);

  // Appends "/chat/completions" or "/api/chat" to the host.
  virtual std::string EndpointPath() const = 0;
  // Names the backend in the request-body debug line.
  virtual std::string LogTag() const = 0;
  // Reads one line of the stream; `content_cb` lets a token reach the user while it is open.
  virtual void ParseLine(std::string_view line,
                         std::function<void(const std::string&)>& content_cb) = 0;
  // What this provider needs, rendered as the request body.
  virtual std::string BuildRequest(const std::vector<ChatMessage>& history,
                                   const std::vector<ToolDefinition>& tools) const = 0;
  // Called before the request is built, so a provider holding fragments clears them first.
  virtual void ResetAccumulators() {}
  // Called when the stream has ended, sentinel or not.
  virtual void FinishStream() {}

  // Content-Type and Authorization, plus whatever a gateway needs beyond them.
  virtual std::vector<std::string> Headers() const;
  // A stream that carries reasoning on a channel of its own; the sink is cleared per call.
  void SetReasoningSink(std::function<void(const std::string&)> sink) {
    reasoning_sink_ = std::move(sink);
  }
  void EmitReasoning(const std::string& text) {
    current_reasoning_content_ += text;
    if (reasoning_sink_) reasoning_sink_(text);
  }

  // What the stream collected, handed back as the result once it ends.
  std::string content_;
  std::string current_reasoning_content_;
  std::string finish_reason_;
  std::string response_model_;
  std::vector<ToolCall> tool_calls_;
  std::optional<TokenUsage> usage_;

 private:
  std::string host_;
  std::string api_key_;
  std::unique_ptr<pu::http::HttpClient> http_;
  std::function<void(const std::string&)> reasoning_sink_;
};

}  // namespace pu
