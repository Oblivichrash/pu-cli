// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/openai_provider.hpp"

#include "pu/llm/projection.hpp"
#include "pu/llm/streaming_json_parser.hpp"
#include "pu/core/platform.hpp"
#include "pu/core/base.hpp"
#include "pu/core/json.hpp"

#include <boost/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <mutex>

namespace pu {

namespace {

std::string SafeString(const boost::json::value& j, const char* key) {
  return (json::HasKey(j, key) && j.at(key).is_string())
             ? boost::json::value_to<std::string>(j.at(key))
             : "";
}

// An error arrives at the top level of a frame, in one of two shapes: the message
// on its own, or an object carrying `message` beside a code and a type. Both are
// read; anything else is kept as it came rather than flattened to nothing.
std::string StreamErrorDetail(const boost::json::value& error) {
  if (error.is_string()) return boost::json::value_to<std::string>(error);
  const std::string message = SafeString(error, "message");
  if (!message.empty()) return message;
  return boost::json::serialize(error);
}

// What this provider needs, as data rather than branches: reasoning is echoed
// back, content is nulled beside tool calls, and arguments travel as a
// JSON-encoded string.
constexpr llm::ProviderCapabilities kCapabilities{
    .role_naming = llm::RoleNaming::kAliasToolResult,
    .echo_reasoning_content = true,
    .allows_content_with_tool_calls = false,
    .tool_arguments = llm::ToolArgumentsEncoding::kJsonString,
    .tool_calls_carry_type = true,
    .sends_tool_name = false,
    .omits_empty_tool_call_id = false,
};

}  // namespace

OpenAIProvider::OpenAIProvider(const Config& config, std::unique_ptr<pu::http::HttpClient> http)
    : config_(config), http_(std::move(http)), host_(config_.host), api_key_(config_.api_key) {}

void OpenAIProvider::ResetAccumulators() {
  pending_tools_.clear();
  current_reasoning_content_.clear();
  refusal_.clear();
  finish_reason_.clear();
  content_.clear();
  tool_calls_.clear();
  usage_.reset();
}

std::string OpenAIProvider::BuildRequest(const std::vector<ChatMessage>& history,
                                         const std::vector<ToolDefinition>& tools) const {
  boost::json::value req = {
      {"model", config_.model},
      {"stream", true},
      // Without this the stream carries no usage object, so the token counts the
      // result reports would never arrive.
      {"stream_options", {{"include_usage", true}}},
      {"temperature", config_.temperature},
      {"max_tokens", config_.max_tokens},
  };

  if (!config_.enable_thinking) {
    boost::json::value extra_body = {{"thinking", {{"type", "disabled"}}}};
    req.as_object()["extra_body"] = extra_body;
  }

  req.as_object()["messages"] = llm::ProjectMessages(history, kCapabilities);

  if (!tools.empty()) {
    boost::json::array tools_json;
    for (const auto& tool : tools) {
      boost::json::value function_obj = {{"name", tool.name},
                                         {"description", tool.description},
                                         {"parameters", tool.Parameters()}};

      tools_json.push_back(boost::json::value{{"type", "function"}, {"function", function_obj}});
    }
    req.as_object()["tools"] = tools_json;
  }
  return boost::json::serialize(req);
}

void OpenAIProvider::HandleJsonToken(const boost::json::value& j,
                                     std::function<void(const std::string&)>& content_cb) {
  // An error can arrive in place of a choice: a request the provider accepted can
  // still be refused while the answer is being generated. The choice path below
  // never sees it, so without this the caller would be handed an empty answer and
  // no reason for it.
  if (json::HasKey(j, "error")) {
    throw Error("provider error: " + StreamErrorDetail(j.at("error")));
  }

  bool is_final = false;
  if (json::HasKey(j, "done") && boost::json::value_to<bool>(j.at("done"))) is_final = true;

  if (json::HasKey(j, "choices") && j.at("choices").is_array() &&
      !j.at("choices").as_array().empty()) {
    const boost::json::value& choice = j.at("choices").at(0);

    // A streaming provider sends `delta`; one that answers in a single frame sends
    // `message`. Reading both is what keeps such a gateway usable without a second
    // path through the parser.
    const boost::json::value* piece = nullptr;
    if (json::HasKey(choice, "delta")) {
      piece = &choice.at("delta");
    } else if (json::HasKey(choice, "message")) {
      piece = &choice.at("message");
    }

    if (piece != nullptr && piece->is_object()) {
      const std::string content = SafeString(*piece, "content");
      if (!content.empty()) {
        content_ += content;
        if (content_cb) content_cb(content);
      }

      // The model's own words when it declines to answer. Dropping them leaves a
      // refusal looking like a backend that said nothing at all.
      const std::string refusal = SafeString(*piece, "refusal");
      if (!refusal.empty()) refusal_ += refusal;

      if (json::HasKey(*piece, "reasoning_content") && piece->at("reasoning_content").is_string()) {
        current_reasoning_content_ +=
            boost::json::value_to<std::string>(piece->at("reasoning_content"));
      }

      if (json::HasKey(*piece, "tool_calls") && piece->at("tool_calls").is_array()) {
        for (const auto& tc : piece->at("tool_calls").as_array()) {
          if (!tc.is_object()) continue;
          const std::string id = SafeString(tc, "id");
          int idx = json::ValueOrDefault<int>(tc, "index", -1);
          if (idx < 0) {
            // Not every provider indexes its fragments. An id marks a call of its
            // own; without one the fragment continues the call already open.
            if (!id.empty()) {
              idx = pending_tools_.empty() ? 0 : pending_tools_.rbegin()->first + 1;
            } else if (!pending_tools_.empty()) {
              idx = pending_tools_.rbegin()->first;
            } else {
              idx = 0;
            }
          }
          auto& acc = pending_tools_[idx];
          if (!id.empty()) acc.id = id;
          if (json::HasKey(tc, "function") && tc.at("function").is_object()) {
            auto name = SafeString(tc.at("function"), "name");
            if (!name.empty()) acc.name = name;
            acc.arguments += SafeString(tc.at("function"), "arguments");
          }
        }
      }
    }

    // Null while the answer is still coming, which is not a reason to stop.
    if (json::HasKey(choice, "finish_reason") && choice.at("finish_reason").is_string()) {
      finish_reason_ = boost::json::value_to<std::string>(choice.at("finish_reason"));
    }
  }

  if (json::HasKey(j, "usage") && j.at("usage").is_object()) {
    const auto& usage = j.at("usage");
    usage_ = TokenUsage{json::ValueOrDefault<int>(usage, "prompt_tokens", 0),
                        json::ValueOrDefault<int>(usage, "completion_tokens", 0)};
  }

  if (is_final) FlushPendingToolCalls();
}

void OpenAIProvider::FlushPendingToolCalls() {
  for (auto& entry : pending_tools_) {
    ToolCallAccumulator& acc = entry.second;
    ToolCall call;
    call.id = acc.id;
    call.name = acc.name;
    if (!acc.arguments.empty()) {
      try {
        call.arguments = boost::json::parse(acc.arguments);
      } catch (const std::exception&) {
        call.arguments = acc.arguments;
      }
    }
    tool_calls_.push_back(std::move(call));
  }
  pending_tools_.clear();
}

ChatResult OpenAIProvider::Chat(const std::vector<ChatMessage>& history,
                                const std::vector<ToolDefinition>& tools,
                                std::function<void(const std::string&)> content_callback,
                                CancelToken cancel_token) {
  ChatResult result;
  platform::ClearInterruptFlag();
  ResetAccumulators();

  const std::string body = BuildRequest(history, tools);

  spdlog::debug("OpenAI request body: {}", body);

  std::string url = host_ + "/chat/completions";
  std::vector<std::string> headers = {"Content-Type: application/json"};
  if (!api_key_.empty()) headers.push_back("Authorization: Bearer " + api_key_);

  llm::StreamingJsonParser parser([&](std::string_view line) {
    constexpr std::string_view kDataPrefix = "data: ";
    auto start = line.find_first_not_of(" \t");
    if (start == std::string_view::npos) return;
    std::string_view trimmed = line.substr(start);
    if (trimmed.substr(0, kDataPrefix.size()) != kDataPrefix) return;
    std::string_view data = trimmed.substr(kDataPrefix.size());
    if (data == "[DONE]") {
      boost::json::value done_obj = {{"done", true}};
      HandleJsonToken(done_obj, content_callback);
      return;
    }
    // Only the parse is guarded. A frame the provider filled with an error is a
    // valid parse and has to reach the caller, which is why the dispatch sits
    // outside the catch that skips malformed lines.
    boost::json::value j;
    try {
      j = boost::json::parse(data);
    } catch (const boost::system::system_error& e) {
      // Skip lines with incomplete/invalid UTF-8 instead of failing the stream.
      spdlog::warn("Skipping invalid JSON line (UTF-8 error): {}", e.what());
      return;
    } catch (const std::exception&) {
      return;
    }
    HandleJsonToken(j, content_callback);
  });

  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (platform::IsInterrupted()) return 0;
    if (cancel_token && cancel_token->load(std::memory_order_acquire)) return 0;
    return total;
  };

  http_->PostStream(url, body, headers, write_cb, cancel_token);

  // A stream that ends without its sentinel still carried what it carried: the
  // fragments held for assembly are calls the provider has already made.
  FlushPendingToolCalls();

  result.content = std::move(content_);
  // A refusal and an answer do not both arrive; when they do, the answer is what
  // the request was for.
  if (result.content.empty()) result.content = std::move(refusal_);
  result.tool_calls = std::move(tool_calls_);
  result.reasoning_content = std::move(current_reasoning_content_);
  result.usage = usage_;
  result.finish_reason = std::move(finish_reason_);
  return result;
}

}  // namespace pu
