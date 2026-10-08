// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/llm/llm_provider.hpp"
#include "pu/core/http_client.hpp"

#include <memory>
#include <optional>
#include <string>

#include <boost/json.hpp>

namespace pu {

class OllamaProvider : public LLMProvider {
 public:
  struct Config {
    std::string host = "http://localhost:11434";
    std::string model;
    float temperature = 0.7f;
    std::string api_key;
    std::string keep_alive = "30m";  // keep the model loaded so the prompt KV cache persists
  };

  explicit OllamaProvider(Config config, std::unique_ptr<pu::http::HttpClient> http);
  ~OllamaProvider() override = default;

  ChatResult Chat(const std::vector<ChatMessage>& history, const std::vector<ToolDefinition>& tools,
                  std::function<void(const std::string&)> content_callback = nullptr,
                  CancelToken cancel_token = nullptr,
                  std::function<void(const std::string&)> reasoning_callback = nullptr) override;

  bool SupportsTools() const override { return true; }
  bool SupportsThinkingLevel() const override { return false; }

 private:
  // A request without tools omits the block rather than carrying an empty one.
  std::string BuildRequest(const std::vector<ChatMessage>& history,
                           const std::vector<ToolDefinition>& tools) const;
  void HandleJsonToken(const boost::json::value& j,
                       std::function<void(const std::string&)>& content_cb);
  void ResetAccumulators();

  Config config_;
  std::string host_;
  std::string api_key_;
  std::unique_ptr<pu::http::HttpClient> http_;
  // Where reasoning tokens go while the stream is open. Held as a member rather
  // than threaded through every helper because only the stream handler reads it,
  // and a provider is built per request (Session::CreateProvider), so no stale
  // sink can outlive the caller it belongs to.
  std::function<void(const std::string&)> reasoning_sink_;
  std::string content_;
  std::string current_reasoning_content_;
  std::string finish_reason_;
  std::string response_model_;
  std::vector<ToolCall> tool_calls_;
  std::optional<TokenUsage> usage_;
};

}  // namespace pu
