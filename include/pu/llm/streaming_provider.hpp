// SPDX-License-Identifier: GPL-3.0-only
#pragma once

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

  ChatResult Chat(const std::vector<ChatMessage>& history,
                  const std::vector<ToolDefinition>& tools,
                  std::function<void(const std::string&)> content_callback = nullptr,
                  CancelToken cancel_token = nullptr,
                  std::function<void(const std::string&)> reasoning_callback = nullptr) final;

  bool SupportsTools() const override { return true; }

 protected:
  StreamingProvider(std::string host, std::string api_key,
                    std::unique_ptr<pu::http::HttpClient> http);

  virtual std::string EndpointPath() const = 0;
  virtual std::string LogTag() const = 0;
  virtual void ParseLine(std::string_view line,
                         std::function<void(const std::string&)>& content_cb) = 0;
  virtual std::string BuildRequest(const std::vector<ChatMessage>& history,
                                   const std::vector<ToolDefinition>& tools) const = 0;
  virtual void ResetAccumulators() {}
  virtual void FinishStream() {}

  virtual std::vector<std::string> Headers() const;
  void SetReasoningSink(std::function<void(const std::string&)> sink) {
    reasoning_sink_ = std::move(sink);
  }
  void EmitReasoning(const std::string& text) {
    current_reasoning_content_ += text;
    if (reasoning_sink_) reasoning_sink_(text);
  }

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
