// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/llm/llm_provider.hpp"
#include "pu/core/http_client.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>

#include <boost/json.hpp>

namespace pu {

class OpenAIProvider : public LLMProvider {
 public:
  struct Config {
    std::string host = "https://api.openai.com/v1";
    std::string model;
    float temperature = 0.7f;
    std::string api_key;
    int max_tokens = 2048;
    bool enable_thinking = true;  // for DeepSeek/vLLM
  };

  explicit OpenAIProvider(const Config& config, std::unique_ptr<pu::http::HttpClient> http);
  ~OpenAIProvider() override = default;

  ChatResult Chat(const std::vector<ChatMessage>& history, const std::vector<ToolDefinition>& tools,
                  std::function<void(const std::string&)> content_callback = nullptr,
                  CancelToken cancel_token = nullptr) override;

  bool SupportsTools() const override { return true; }
  bool IsThinkingMode() const override { return config_.enable_thinking; }

 private:
  // A request without tools omits the block rather than carrying an empty one.
  std::string BuildRequest(const std::vector<ChatMessage>& history,
                           const std::vector<ToolDefinition>& tools) const;
  void HandleJsonToken(const boost::json::value& j,
                       std::function<void(const std::string&)>& content_cb);
  void ResetAccumulators();

  Config config_;
  std::unique_ptr<pu::http::HttpClient> http_;
  std::string host_;
  std::string api_key_;

  struct ToolCallAccumulator {
    std::string id, name, arguments;
  };
  std::map<int, ToolCallAccumulator> pending_tools_;

  std::string content_;
  std::string current_reasoning_content_;
  std::vector<ToolCall> tool_calls_;
  std::optional<TokenUsage> usage_;
};

}  // namespace pu
