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
    std::optional<int> max_tokens;
    ThinkingLevel thinking = ThinkingLevel::kServerDefault;
    std::function<std::vector<std::string>()> extra_headers;
  };

  explicit OpenAIProvider(const Config& config, std::shared_ptr<pu::http::HttpClient> http);
  ~OpenAIProvider() override = default;

 protected:
  std::string EndpointPath() const override { return "/chat/completions"; }
  std::string LogTag() const override { return "OpenAI"; }
  std::vector<std::string> Headers() const override;

  std::string BuildRequest(std::span<const ChatMessage> history,
                           std::span<const ToolDefinition> tools) const override;
  void ParseLine(std::string_view line, const ChatRequest& request) override;
  void ResetAccumulators() override;
  void FinishStream(const ChatRequest& request) override;

 private:
  void HandleJsonToken(const boost::json::value& j, const ChatRequest& request);
  void FlushPendingToolCalls(const ChatRequest& request);

  Config config_;

  struct ToolCallAccumulator {
    std::string id, name, arguments;
  };
  std::vector<ToolCallAccumulator> pending_tools_;

  // The gateway's refusal text, kept apart from `content_` until the stream ends: a
  // reply can arrive with both, and folding one into the other mid-stream would make
  // the two indistinguishable.
  std::string refusal_;
};

}  // namespace pu
