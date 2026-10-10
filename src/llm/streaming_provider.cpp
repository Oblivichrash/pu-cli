// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/streaming_provider.hpp"

#include "pu/llm/streaming_json_parser.hpp"
#include "pu/core/platform.hpp"

#include <spdlog/spdlog.h>

namespace pu {

StreamingProvider::StreamingProvider(std::string host, std::string api_key,
                                     std::shared_ptr<pu::http::HttpClient> http)
    : host_(std::move(host)), api_key_(std::move(api_key)), http_(std::move(http)) {}

std::vector<std::string> StreamingProvider::Headers() const {
  std::vector<std::string> headers = {"Content-Type: application/json"};
  if (!api_key_.empty()) headers.push_back("Authorization: Bearer " + api_key_);
  return headers;
}

void StreamingProvider::AppendContent(std::string_view text, const ChatRequest& request) {
  if (text.empty()) return;
  content_ += text;
  if (request.on_content) request.on_content(text);
}

void StreamingProvider::AppendReasoning(std::string_view text, const ChatRequest& request,
                                        std::string signature, boost::json::value raw) {
  if (text.empty()) return;

  const bool starts_block = !signature.empty() || !raw.is_null();
  if (reasoning_.empty() || starts_block) {
    ReasoningBlock block;
    block.signature = std::move(signature);
    if (!raw.is_null()) block.raw = std::move(raw);
    reasoning_.push_back(std::move(block));
  }

  ReasoningBlock& block = reasoning_.back();
  block.text.append(text);

  if (request.on_reasoning) request.on_reasoning(text);
  if (request.on_reasoning_block) request.on_reasoning_block(block);
}

void StreamingProvider::AppendToolCall(ToolCall call, const ChatRequest& request) {
  tool_calls_.push_back(std::move(call));
  if (request.on_tool_call) request.on_tool_call(tool_calls_.back());
}

ChatResult StreamingProvider::Chat(const ChatRequest& request) {
  ChatResult result;
  platform::ClearInterruptFlag();
  ResetAccumulators();

  const std::string body = BuildRequest(*request.history, *request.tools);
  spdlog::debug("{} request body: {}", LogTag(), body);

  std::vector<std::string> headers = Headers();

  llm::StreamingJsonParser parser([&](std::string_view line) { ParseLine(line, request); });

  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (platform::IsInterrupted()) return 0;
    if (request.cancel_token && request.cancel_token->load(std::memory_order_acquire)) return 0;
    return total;
  };

  http_->PostStream(host_ + EndpointPath(), body, headers, write_cb, request.cancel_token);

  FinishStream(request);

  result.content = std::move(content_);
  result.tool_calls = std::move(tool_calls_);
  result.reasoning = std::move(reasoning_);
  result.usage = usage_;
  result.finish_reason = std::move(finish_reason_);
  result.model = std::move(response_model_);
  return result;
}

}  // namespace pu
