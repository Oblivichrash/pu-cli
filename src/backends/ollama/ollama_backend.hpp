// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/backend.hpp"
#include "pu/http/http_client.hpp"
#include "pu/token_adapter.hpp"
#include <memory>
#include <string>

namespace pu::backends::ollama {

class OllamaBackend : public pu::backend::Backend {
 public:
  struct Config : public pu::backend::Backend::Config {
    std::string host = "http://localhost:11434";
    std::string api_key;
  };

  explicit OllamaBackend(Config config, std::unique_ptr<pu::http::HttpClient> http,
                         std::unique_ptr<ITokenAdapter> adapter);
  ~OllamaBackend() override = default;

  void Chat(const std::vector<pu::backend::Message>& history,
            pu::backend::ChatCallback cb) override;
  void Chat(const std::vector<pu::backend::Message>& history,
            const std::vector<pu::backend::ToolDefinition>& tools,
            pu::backend::ChatCallback content_cb, pu::backend::ToolCallback tool_cb) override;
  bool SupportsTools() const override { return true; }

 private:
  std::string BuildRequest(const std::vector<pu::backend::Message>& history) const;
  std::string BuildRequestWithTools(const std::vector<pu::backend::Message>& history,
                                    const std::vector<pu::backend::ToolDefinition>& tools) const;
  std::string host_;
  std::string api_key_;
  std::unique_ptr<pu::http::HttpClient> http_;
  std::unique_ptr<ITokenAdapter> adapter_;
};

}  // namespace pu::backends::ollama
