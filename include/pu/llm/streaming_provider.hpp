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

  ChatResult Chat(const ChatRequest& request) final;

  bool SupportsTools() const override { return true; }

 protected:
  StreamingProvider(std::string host, std::string api_key,
                    std::shared_ptr<pu::http::HttpClient> http);

  virtual std::string EndpointPath() const = 0;
  virtual std::string LogTag() const = 0;
  virtual void ParseLine(std::string_view line, const ChatRequest& request) = 0;
  virtual std::string BuildRequest(const std::vector<ChatMessage>& history,
                                   const std::vector<ToolDefinition>& tools) const = 0;
  virtual void ResetAccumulators() {}
  virtual void FinishStream(const ChatRequest& request) { (void)request; }

  virtual std::vector<std::string> Headers() const;

  void AppendContent(std::string_view text, const ChatRequest& request);
  void AppendReasoning(std::string_view text, const ChatRequest& request, std::string signature = {},
                       boost::json::value raw = nullptr);
  void AppendToolCall(ToolCall call, const ChatRequest& request);

  std::string content_;
  std::vector<ReasoningBlock> reasoning_;
  std::vector<ToolCall> tool_calls_;
  std::string finish_reason_;
  std::string response_model_;
  std::optional<TokenUsage> usage_;

 private:
  std::string host_;
  std::string api_key_;
  std::shared_ptr<pu::http::HttpClient> http_;
};

}  // namespace pu
