// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/llm/llm_provider.hpp"
#include "pu/http/http_client.hpp"

#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace pu {

class OpenAIProvider : public LLMProvider {
 public:
  struct Config {
    std::string host = "https://api.openai.com/v1";
    std::string model;
    float temperature = 0.7f;
    std::optional<std::string> system_prompt;
    std::string api_key;
    bool parameters_as_string = false;
    int max_tokens = 2048;
  };

  explicit OpenAIProvider(const Config& config,
                          std::unique_ptr<pu::http::HttpClient> http);
  ~OpenAIProvider() override = default;

  ChatResult Chat(
    const std::vector<ChatMessage>& history,
    const std::vector<ToolDefinition>& tools,
    std::function<void(const std::string&)> content_callback = nullptr,
    std::function<void(const ToolCall&)> tool_callback = nullptr
  ) override;

  bool SupportsTools() const override { return true; }
  bool SupportsStrictMode() const override { return true; }
  std::string GetModelName() const override { return config_.model; }

 private:
  std::string BuildRequest(const std::vector<ChatMessage>& history) const;
  std::string BuildRequestWithTools(const std::vector<ChatMessage>& history,
                                    const std::vector<ToolDefinition>& tools) const;
  void HandleJsonToken(const nlohmann::json& j,
                       std::function<void(const std::string&)>& content_cb,
                       std::function<void(const ToolCall&)>& tool_cb);
  void ResetAccumulators();

  Config config_;
  std::unique_ptr<pu::http::HttpClient> http_;
  std::string host_;
  std::string api_key_;

  struct ToolCallAccumulator { std::string id, name, arguments; };
  std::map<int, ToolCallAccumulator> pending_tools_;

  std::string current_trace_id_;
  std::string current_reasoning_content_;
};

}  // namespace pu
