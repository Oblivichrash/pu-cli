// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/core/http_client.hpp"
#include "pu/config/gateway.hpp"
#include "pu/llm/openai_protocol.hpp"
#include "pu/llm/streaming_provider.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace pu::llm {

// The names a provider gives the fields on a reply, as a counterpart to the names it
// gives them on a request. An empty name means the field does not exist on that gateway.
struct ResponseShape {
  std::string_view content_field = "content";
  std::string_view reasoning_field = "reasoning_content";
  std::string_view refusal_field = "refusal";

  std::string_view usage_field = "usage";
  std::string_view prompt_tokens_field = "prompt_tokens";
  std::string_view completion_tokens_field = "completion_tokens";
  std::string_view total_tokens_field = "total_tokens";
  // What a call cost is a product fact, not a protocol one: two products share this shape and
  // only one of them reports a price. `config::Gateway::billing` says which, and where.

  // One gateway nests the cached prompt, another puts it at the top level; both carry the
  // same count. The nested name is tried first.
  std::string_view cached_tokens_parent = "prompt_tokens_details";
  std::string_view cached_tokens_nested_field = "cached_tokens";
  std::string_view cached_tokens_top_level_field = "prompt_cache_hit_tokens";

  std::string_view finish_reason_field = "finish_reason";
};

// The shape's field names a reply never carried. `required_only` is what the runtime can act
// on, since `content` and `reasoning_content` are legitimately absent from a tool-call reply
// and from a model that does not reason; a capture asks about those too.
std::vector<std::string> AbsentShapeFields(
    const ResponseShape& shape, const std::function<bool(std::string_view)>& seen,
    bool required_only);

}  // namespace pu::llm

namespace pu {

// The OpenAI wire format: the path a turn is posted to, the body it is asked in, and the
// SSE stream it is answered with. It is a layer of the provider rather than a thing beside
// it because it reads the stream through the pipeline's own virtuals and writes into the
// pipeline's accumulators; what it does not hold is a host or a set of headers, which is
// what a gateway states and what OpenAIProvider adds.
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
    // How the product that built this provider prices a call. Default `kNone`: a provider
    // built without a gateway behind it — a test, a probe — must not invent a price.
    config::Billing billing;
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
