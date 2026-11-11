// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/streaming_provider.hpp"

#include "pu/core/sse.hpp"
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
  acc_.content += text;
  if (request.on_content) request.on_content(text);
}

void StreamingProvider::AppendReasoning(std::string_view text, const ChatRequest& request,
                                        std::string signature, boost::json::value raw) {
  if (text.empty()) return;

  const bool starts_block = !signature.empty() || !raw.is_null();
  if (acc_.reasoning.empty() || starts_block) {
    ReasoningBlock block;
    block.signature = std::move(signature);
    if (!raw.is_null()) block.raw = std::move(raw);
    acc_.reasoning.push_back(std::move(block));
  }

  ReasoningBlock& block = acc_.reasoning.back();
  block.text.append(text);

  if (request.on_reasoning) request.on_reasoning(text);
}

void StreamingProvider::AppendToolCall(ToolCall call, const ChatRequest& request) {
  acc_.tool_calls.push_back(std::move(call));
  if (request.on_tool_call) request.on_tool_call(acc_.tool_calls.back());
}

void StreamingProvider::NoteUnreadFields(const boost::json::value& j,
                                         std::span<const std::string_view> read_keys) {
  // One frame is enough: a stream repeats its shape, and walking every one of 300 frames
  // would put this on the hot path to learn what the first already said.
  if (acc_.inspected_frame) return;

  const boost::json::object* obj = j.if_object();
  if (obj == nullptr) return;
  acc_.inspected_frame = true;

  for (const auto& member : *obj) {
    const std::string_view key = member.key();
    const bool read = std::any_of(read_keys.begin(), read_keys.end(), [key](std::string_view name) {
      return name == key;
    });
    if (!read) acc_.unread_fields.emplace_back(key);
  }
}

ChatResult StreamingProvider::Accumulators::IntoResult() && {
  ChatResult result;
  result.content = std::move(content);
  result.tool_calls = std::move(tool_calls);
  result.reasoning = std::move(reasoning);
  result.usage = usage;
  result.raw_usage = std::move(raw_usage);
  result.unread_fields = std::move(unread_fields);
  result.finish_reason = std::move(finish_reason);
  result.model = std::move(response_model);
  return result;
}

ChatResult StreamingProvider::Chat(const ChatRequest& request) {
  acc_ = Accumulators{};
  ResetTurnState();

  const std::string body = BuildRequest(request.history, request.tools);
  // The body itself is the conversation, which session.json already carries; logging
  // it whole would double every tool output the transcript keeps.
  spdlog::debug("{} request: {} bytes", LogTag(), body.size());

  std::vector<std::string> headers = Headers();

  core::SseParser parser([&](const core::SseEvent& event) {
    // A frame that carried no data is a name with nothing to deliver. The `[DONE]` sentinel is
    // the sender's last word and nothing follows it, so it is skipped: finishing the turn is
    // what the end of the stream does, once, below. Doing it here as well ran the flush twice
    // and appended what it folded in twice.
    if (event.data.empty() || event.data == "[DONE]") return;
    ParseData(event.data, request);
  });
  parser.SetBareLinesAreEvents(FramesBareLines());

  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (request.cancel_token && request.cancel_token->load(std::memory_order_acquire)) return 0;
    return total;
  };

  http_->PostStream(host_ + EndpointPath(), body, headers, write_cb, request.cancel_token);

  // A frame that was never closed by a blank line is still a frame, and a `[DONE]` sent
  // without one is still the end: the close terminates the last line.
  parser.Finish();
  FinishStream(request);

  return std::move(acc_).IntoResult();
}

}  // namespace pu
