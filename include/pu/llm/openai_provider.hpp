// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/core/http_client.hpp"
#include "pu/llm/streaming_provider.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace pu {

class OpenAIProvider : public StreamingProvider {
 public:
  struct Config {
    std::string host = "https://api.openai.com/v1";
    std::string model;
    float temperature = 0.7f;
    std::string api_key;
    int max_tokens = 2048;
    ThinkingLevel thinking = ThinkingLevel::kServerDefault;
    std::function<std::vector<std::string>()> extra_headers;
  };

  explicit OpenAIProvider(const Config& config, std::unique_ptr<pu::http::HttpClient> http);
  ~OpenAIProvider() override = default;

  bool SupportsThinkingLevel() const override { return true; }

 protected:
  std::string EndpointPath() const override { return "/chat/completions"; }
  std::string LogTag() const override { return "OpenAI"; }
  std::vector<std::string> Headers() const override;

  std::string BuildRequest(const std::vector<ChatMessage>& history,
                           const std::vector<ToolDefinition>& tools) const override;
  void ParseLine(std::string_view line,
                 std::function<void(const std::string&)>& content_cb) override;
  void ResetAccumulators() override;
  void FinishStream() override;

 private:
  void HandleJsonToken(const boost::json::value& j,
                       std::function<void(const std::string&)>& content_cb);
  void FlushPendingToolCalls();

  Config config_;

  struct ToolCallAccumulator {
    std::string id, name, arguments;
  };
  std::vector<ToolCallAccumulator> pending_tools_;

  std::string refusal_;
};

}  // namespace pu
