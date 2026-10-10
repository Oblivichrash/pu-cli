// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/core/http_client.hpp"
#include "pu/llm/streaming_provider.hpp"

#include <memory>
#include <string>

#include <boost/json.hpp>

namespace pu {

class OllamaProvider : public StreamingProvider {
 public:
  struct Config {
    std::string host = "http://localhost:11434";
    std::string model;
    float temperature = 0.7f;
    std::string api_key;
    std::string keep_alive = "30m";  // keep the model loaded so the prompt KV cache persists
  };

  explicit OllamaProvider(Config config, std::shared_ptr<pu::http::HttpClient> http);
  ~OllamaProvider() override = default;

 protected:
  std::string EndpointPath() const override { return "/api/chat"; }
  std::string LogTag() const override { return "Ollama"; }

  std::string BuildRequest(const std::vector<ChatMessage>& history,
                           const std::vector<ToolDefinition>& tools) const override;
  void ParseLine(std::string_view line, const ChatRequest& request) override;
  void ResetAccumulators() override;

 private:
  void HandleJsonToken(const boost::json::value& j, const ChatRequest& request);

  Config config_;
};

}  // namespace pu
