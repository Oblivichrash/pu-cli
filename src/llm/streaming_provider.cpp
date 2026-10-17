// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/streaming_provider.hpp"

#include "pu/llm/streaming_json_parser.hpp"
#include "pu/core/platform.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace pu {

StreamingProvider::StreamingProvider(std::string host, std::string api_key,
                                     std::shared_ptr<pu::http::HttpClient> http)
    : host_(std::move(host)), api_key_(std::move(api_key)), http_(std::move(http)) {}

std::vector<std::string> StreamingProvider::Headers() const {
  std::vector<std::string> headers = {"Content-Type: application/json"};
  const std::string bearer = http::BearerHeader(api_key_);
  if (!bearer.empty()) headers.push_back(bearer);
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
}

void StreamingProvider::AppendToolCall(ToolCall call, const ChatRequest& request) {
  tool_calls_.push_back(std::move(call));
  if (request.on_tool_call) request.on_tool_call(tool_calls_.back());
}

void StreamingProvider::NoteUnreadFields(const boost::json::value& j,
                                         std::span<const std::string_view> read_keys) {
  // One frame is enough: a stream repeats its shape, and walking every one of 300 frames
  // would put this on the hot path to learn what the first already said.
  if (inspected_frame_) return;

  const boost::json::object* obj = j.if_object();
  if (obj == nullptr) return;
  inspected_frame_ = true;

  for (const auto& member : *obj) {
    const std::string_view key = member.key();
    const bool read = std::any_of(read_keys.begin(), read_keys.end(), [key](std::string_view name) {
      return name == key;
    });
    if (!read) unread_fields_.emplace_back(key);
  }
}

void StreamingProvider::NoteSeenField(std::string_view key) {
  if (std::find(seen_fields_.begin(), seen_fields_.end(), key) == seen_fields_.end()) {
    seen_fields_.emplace_back(key);
  }
}

ChatResult StreamingProvider::Chat(const ChatRequest& request) {
  ChatResult result;
  ResetAccumulators();

  const std::string body = BuildRequest(request.history, request.tools);
  // The body itself is the conversation, which session.json already carries; logging
  // it whole would double every tool output the transcript keeps.
  spdlog::debug("{} request: {} bytes", LogTag(), body.size());

  std::vector<std::string> headers = Headers();

  llm::StreamingJsonParser parser([&](std::string_view line) { ParseLine(line, request); });

  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (request.cancel_token && request.cancel_token->load(std::memory_order_acquire)) return 0;
    return total;
  };

  http_->PostStream(host_ + EndpointPath(), body, headers, write_cb, request.cancel_token);

  FinishStream(request);

  result.content = std::move(content_);
  result.tool_calls = std::move(tool_calls_);
  result.reasoning = std::move(reasoning_);
  result.usage = usage_;
  result.raw_usage = std::move(raw_usage_);
  result.unread_fields = std::move(unread_fields_);
  result.seen_fields = std::move(seen_fields_);
  result.missing_fields = std::move(missing_fields_);
  result.finish_reason = std::move(finish_reason_);
  result.model = std::move(response_model_);
  return result;
}

}  // namespace pu
