// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/llm/llm_provider.hpp"
#include "pu/http_client.hpp"

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
    std::optional<std::string> system_prompt;
    std::string api_key;
    int max_tokens = 2048;
    std::string keep_alive = "30m";  // keep the model loaded so the prompt KV cache persists
  };

  explicit OllamaProvider(Config config, std::unique_ptr<pu::http::HttpClient> http);
  ~OllamaProvider() override = default;

  ChatResult Chat(
    const std::vector<ChatMessage>& history,
    const std::vector<ToolDefinition>& tools,
    std::function<void(const std::string&)> content_callback = nullptr,
    std::function<void(const ToolCall&)> tool_callback = nullptr,
    CancelToken cancel_token = nullptr
  ) override;

  bool SupportsTools() const override { return true; }
  std::string GetModelName() const override { return config_.model; }
  bool IsThinkingMode() const override { return false; }

 private:
  std::string BuildRequest(const std::vector<ChatMessage>& history) const;
  std::string BuildRequestWithTools(const std::vector<ChatMessage>& history,
                                    const std::vector<ToolDefinition>& tools) const;
  void HandleJsonToken(const boost::json::value& j,
                       std::function<void(const std::string&)>& content_cb,
                       std::function<void(const ToolCall&)>& tool_cb);
  std::string RoleToString(const std::string& role) const;

  Config config_;
  std::string host_;
  std::string api_key_;
  std::unique_ptr<pu::http::HttpClient> http_;
};

}  // namespace pu
