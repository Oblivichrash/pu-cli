// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// OpenAI-compatible backend implementation.

#pragma once

#include "pu/backend.hpp"
#include "pu/http/http_client.hpp"
#include <memory>
#include <string>

namespace pu::backends::openai {

/// OpenAI-compatible chat completion backend.
/// Supports any service that implements the OpenAI /v1/chat/completions
/// API with SSE streaming (e.g., DeepSeek, Qwen, Doubao, local vLLM).
/// Requires an API key unless the endpoint is unauthenticated.
class OpenAIBackend : public pu::backend::Backend {
 public:
  struct Config : public pu::backend::Backend::Config {
    std::string host = "https://api.openai.com";
    std::string api_key;  // Required for OpenAI API
    
    Config() = default;
  };

  explicit OpenAIBackend(const Config& config,
                         std::unique_ptr<pu::http::HttpClient> http);
  ~OpenAIBackend() override = default;

  void Chat(const std::vector<pu::backend::Message>& history,
            pu::backend::ChatCallback cb) override;

  void Chat(const std::vector<pu::backend::Message>& history,
            const std::vector<pu::backend::ToolDefinition>& tools,
            pu::backend::ChatCallback content_cb,
            pu::backend::ToolCallback tool_cb) override;

 private:
  std::string BuildRequest(const std::vector<pu::backend::Message>& history) const;

  std::unique_ptr<pu::http::HttpClient> http_;
  std::string host_;
  std::string api_key_;
};

}  // namespace pu::backends::openai
