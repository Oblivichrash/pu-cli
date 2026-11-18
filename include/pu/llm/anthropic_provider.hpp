// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// The Anthropic Messages wire format.
//
// It differs from the OpenAI one in four ways that are not renamings:
//
//   * content is a list of blocks (`{type:"text"}` and `{type:"tool_use"}` on the request,
//     `content_block_start` / `content_block_delta` events on the reply) rather than a string
//     beside a `tool_calls` array;
//   * the reply is dispatched on the SSE `event:` name, which OpenAI never sets;
//   * the system prompt is a top-level field rather than a message with a role;
//   * `max_tokens` is required rather than optional.
//
// The in-memory conversation is a string plus a `tool_calls` array, so this provider is the
// one place the two representations meet: it builds the block list from the message it is
// given and folds the block events back into a string and a call list. That translation is
// why it lives in one file of its own rather than sharing the OpenAI parser with a name table
// — the shapes are not the same shape with different names.
//

#include "pu/core/http_client.hpp"
#include "pu/llm/streaming_provider.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace pu {

class AnthropicProvider : public StreamingProvider {
 public:
  // Anthropic states its API version in a header every request carries, and the value is a
  // date rather than a number: the product's own, not a caller's.
  static constexpr const char* kApiVersion = "2023-06-01";

  struct Config {
    std::string host = "https://api.anthropic.com/v1";
    std::string model;
    float temperature = 0.7f;
    std::string api_key;
    // Required by this API: an absent cap is a refused request, not a default. The caller
    // names one or this build would have to invent a number the product never agreed to.
    int max_tokens = 8192;
    std::optional<float> top_p;
    std::vector<std::string> stop;
    ThinkingLevel thinking = ThinkingLevel::kServerDefault;
    // The system prompt, sent as the top-level `system` field rather than as a message.
    std::string system_prompt;
  };

  explicit AnthropicProvider(Config config, std::shared_ptr<pu::http::HttpClient> http);
  ~AnthropicProvider() override = default;

 protected:
  std::string EndpointPath() const override { return "/messages"; }
  std::string LogTag() const override { return "Anthropic"; }

  std::vector<std::string> Headers() const override;
  std::string BuildRequest(std::span<const ChatMessage> history,
                           std::span<const ToolDefinition> tools) const override;
  // The block events are one open block at a time, and which block is open is turn state
  // rather than a message field, so it is cleared with the rest of the turn.
  void ParseData(std::string_view event, std::string_view data,
                 const ChatRequest& request) override;
  void ResetTurnState() override;
  void FinishStream(const ChatRequest& request) override;

 private:
  void HandleEvent(std::string_view event, const boost::json::value& j,
                   const ChatRequest& request);
  void HandleContentBlockStart(const boost::json::value& j, const ChatRequest& request);
  void HandleContentBlockDelta(const boost::json::value& j, const ChatRequest& request);
  void HandleMessageDelta(const boost::json::value& j);
  void CloseOpenBlock(const ChatRequest& request);

  Config config_;

  // Which kind of block the stream is in the middle of, and the tool call it is building.
  // `content_block_stop` closes it; a `message_stop` closes it whether or not that arrived.
  enum class OpenBlock { kNone, kText, kThinking, kToolUse, kOther };
  struct OpenToolUse {
    std::string id;
    std::string name;
    std::string arguments;
  };

  OpenBlock open_ = OpenBlock::kNone;
  OpenToolUse open_tool_;
  // The thinking block's signature arrives on `content_block_delta` as `signature_delta`,
  // after the text, and has to be replayed whole on the next turn. Kept beside the text the
  // shared accumulator is building so both land in one `ReasoningBlock`.
  std::string pending_signature_;
};

}  // namespace pu
