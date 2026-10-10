// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/streaming_provider.hpp"

#include "pu/llm/streaming_json_parser.hpp"
#include "pu/core/platform.hpp"

#include <spdlog/spdlog.h>

namespace pu {

StreamingProvider::StreamingProvider(std::string host, std::string api_key,
                                     std::unique_ptr<pu::http::HttpClient> http)
    : host_(std::move(host)), api_key_(std::move(api_key)), http_(std::move(http)) {}

std::vector<std::string> StreamingProvider::Headers() const {
  std::vector<std::string> headers = {"Content-Type: application/json"};
  if (!api_key_.empty()) headers.push_back("Authorization: Bearer " + api_key_);
  return headers;
}

ChatResult StreamingProvider::Chat(const std::vector<ChatMessage>& history,
                                   const std::vector<ToolDefinition>& tools,
                                   std::function<void(const std::string&)> content_callback,
                                   CancelToken cancel_token,
                                   std::function<void(const std::string&)> reasoning_callback) {
  ChatResult result;
  platform::ClearInterruptFlag();
  ResetAccumulators();
  SetReasoningSink(std::move(reasoning_callback));

  const std::string body = BuildRequest(history, tools);
  spdlog::debug("{} request body: {}", LogTag(), body);

  std::vector<std::string> headers = Headers();

  llm::StreamingJsonParser parser([&](std::string_view line) { ParseLine(line, content_callback); });

  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (platform::IsInterrupted()) return 0;
    if (cancel_token && cancel_token->load(std::memory_order_acquire)) return 0;
    return total;
  };

  http_->PostStream(host_ + EndpointPath(), body, headers, write_cb, cancel_token);

  FinishStream();

  result.content = std::move(content_);
  result.tool_calls = std::move(tool_calls_);
  result.reasoning_content = std::move(current_reasoning_content_);
  result.usage = usage_;
  result.finish_reason = std::move(finish_reason_);
  result.model = std::move(response_model_);
  return result;
}

}  // namespace pu
