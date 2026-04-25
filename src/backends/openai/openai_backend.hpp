// SPDX-License-Identifier: GPL-3.0-only
//
// OpenAI-compatible backend implementation.

#pragma once

#include "pu/backend.hpp"
#include "pu/http/http_client.hpp"
#include <memory>
#include <string>

namespace pu::backends::openai {

class OpenAIBackend : public pu::backend::Backend {
 public:
  struct Config : public pu::backend::Backend::Config {
    std::string host = "https://api.openai.com";
    std::string api_key;

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
