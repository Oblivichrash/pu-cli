// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/backend.hpp"
#include "pu/http/http_client.hpp"
#include <nlohmann/json.hpp>
#include <map>
#include <memory>
#include <string>

namespace pu::backends::openai {

class OpenAIBackend : public pu::backend::Backend {
 public:
  struct Config : public pu::backend::Backend::Config {
    std::string host = "https://api.openai.com/v1";
    std::string api_key;
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
  bool SupportsTools() const override { return true; }

 private:
  std::string BuildRequest(const std::vector<pu::backend::Message>& history) const;
  std::string BuildRequestWithTools(const std::vector<pu::backend::Message>& history,
                                    const std::vector<pu::backend::ToolDefinition>& tools) const;
  void HandleJsonToken(const nlohmann::json& j, pu::backend::ChatCallback content_cb,
                       pu::backend::ToolCallback tool_cb);
  void ResetAccumulators();

  std::unique_ptr<pu::http::HttpClient> http_;
  std::string host_;
  std::string api_key_;

  struct ToolCallAccumulator { std::string id, name, arguments; };
  std::map<int, ToolCallAccumulator> pending_tools_;
};

}  // namespace pu::backends::openai
