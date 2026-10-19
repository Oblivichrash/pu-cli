// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "pu/core/http_client.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu {

class StreamingProvider : public LLMProvider {
 public:
  ~StreamingProvider() override = default;

  ChatResult Chat(const ChatRequest& request) final;

 protected:
  StreamingProvider(std::string host, std::string api_key,
                    std::shared_ptr<pu::http::HttpClient> http);

  virtual std::string EndpointPath() const = 0;
  virtual std::string LogTag() const = 0;
  // One event's payload. The framing — which lines are data, where an event ends — belongs to
  // the format, and `core::SseParser` owns it; a provider reads payloads.
  virtual void ParseData(std::string_view data, const ChatRequest& request) = 0;

  // Whether this gateway frames events or writes one JSON object per line. The parser is told,
  // so a stream that carries no `data:` prefix is still read as the format it is.
  virtual bool FramesBareLines() const { return false; }
  virtual std::string BuildRequest(std::span<const ChatMessage> history,
                                   std::span<const ToolDefinition> tools) const = 0;
  virtual void ResetAccumulators() {}
  virtual void FinishStream(const ChatRequest& request) { (void)request; }

  virtual std::vector<std::string> Headers() const;

  void AppendContent(std::string_view text, const ChatRequest& request);
  void AppendReasoning(std::string_view text, const ChatRequest& request, std::string signature = {},
                       boost::json::value raw = nullptr);
  void AppendToolCall(ToolCall call, const ChatRequest& request);

  // Notes the keys on `j` that no entry of `read_keys` names. A gateway adding a field is
  // not an error, so nothing is thrown; the names are collected for a caller to look at
  // and a test to assert, which is what would have caught a count that was read nowhere.
  // A gateway names a field once per stream, so a stream's frames are not walked twice.
  void NoteUnreadFields(const boost::json::value& j, std::span<const std::string_view> read_keys);

  // The other half of NoteUnreadFields: a gateway that renamed `content` would otherwise
  // answer with an empty reply and no complaint.
  void NoteSeenField(std::string_view key);

  std::string content_;
  std::vector<ReasoningBlock> reasoning_;
  std::vector<ToolCall> tool_calls_;
  std::string finish_reason_;
  std::string response_model_;
  std::optional<TokenUsage> usage_;
  boost::json::value raw_usage_;
  std::vector<std::string> unread_fields_;
  std::vector<std::string> seen_fields_;
  std::vector<std::string> missing_fields_;
  bool inspected_frame_ = false;

 private:
  std::string host_;
  std::string api_key_;
  std::shared_ptr<pu::http::HttpClient> http_;
};

}  // namespace pu
