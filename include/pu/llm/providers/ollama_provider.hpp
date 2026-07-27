// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/llm/llm_provider.hpp"
#include "pu/http/http_client.hpp"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

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
  };

  explicit OllamaProvider(Config config, std::unique_ptr<pu::http::HttpClient> http);
  ~OllamaProvider() override = default;

  ChatResult Chat(
    const std::vector<ChatMessage>& history,
    const std::vector<ToolDefinition>& tools,
    std::function<void(const std::string&)> content_callback = nullptr,
    std::function<void(const ToolCall&)> tool_callback = nullptr
  ) override;

  bool SupportsTools() const override { return true; }
  bool SupportsStrictMode() const override { return false; }
  std::string GetModelName() const override { return config_.model; }

 private:
  std::string BuildRequest(const std::vector<ChatMessage>& history) const;
  std::string BuildRequestWithTools(const std::vector<ChatMessage>& history,
                                    const std::vector<ToolDefinition>& tools) const;
  void HandleJsonToken(const nlohmann::json& j,
                       std::function<void(const std::string&)>& content_cb,
                       std::function<void(const ToolCall&)>& tool_cb);
  std::string RoleToString(const std::string& role) const;
  ChatMessage ToChatMessage(const nlohmann::json& msg) const;

  Config config_;
  std::string host_;
  std::string api_key_;
  std::unique_ptr<pu::http::HttpClient> http_;
};

}  // namespace pu
