// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/core/http_client.hpp"
#include "pu/llm/response_shape.hpp"
#include "pu/llm/streaming_provider.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace pu {

// The OpenAI wire format: the path a turn is posted to, the body it is asked in, and the
// SSE stream it is answered with. It is a layer of the provider rather than a thing beside
// it because it reads the stream through the pipeline's own virtuals and writes into the
// pipeline's accumulators; what it does not hold is a host or a set of headers, which is
// what a gate states and what OpenAIProvider adds.
class OpenAIProtocol : public StreamingProvider {
 public:
  struct Config {
    std::string host = "https://api.openai.com/v1";
    std::string model;
    float temperature = 0.7f;
    std::string api_key;
    std::optional<int> max_tokens;
    ThinkingLevel thinking = ThinkingLevel::kServerDefault;
    std::function<std::vector<std::string>()> extra_headers;
    llm::ResponseShape shape;
  };

 protected:
  OpenAIProtocol(const Config& config, std::shared_ptr<pu::http::HttpClient> http);

  std::string EndpointPath() const override { return "/chat/completions"; }
  std::string LogTag() const override { return "OpenAI"; }

  std::string BuildRequest(std::span<const ChatMessage> history,
                           std::span<const ToolDefinition> tools) const override;
  void ParseLine(std::string_view line, const ChatRequest& request) override;
  void ResetAccumulators() override;
  void FinishStream(const ChatRequest& request) override;

 private:
  void HandleJsonToken(const boost::json::value& j, const ChatRequest& request);
  void FlushPendingData(const ChatRequest& request);
  void FlushPendingToolCalls(const ChatRequest& request);

  Config config_;

  struct ToolCallAccumulator {
    std::string id, name, arguments;
  };
  std::vector<ToolCallAccumulator> pending_tools_;

  // One SSE payload can arrive as several `data:` lines; a payload that parses is the
  // whole of it, and a blank line closes the event either way.
  std::string pending_data_;

  // The request asks for one completion, so the choice the reply is read from is the
  // first index the stream names; another index is a gateway sending more than was asked
  // for, and is reported rather than folded into the reply.
  int reply_choice_index_ = -1;
  bool reported_extra_choice_ = false;

  // The gateway's refusal text, kept apart from `content_` until the stream ends: a
  // reply can arrive with both, and folding one into the other mid-stream would make
  // the two indistinguishable.
  std::string refusal_;
};

}  // namespace pu
