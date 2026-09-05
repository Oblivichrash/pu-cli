// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/openai_provider.hpp"

#include "pu/llm/streaming_json_parser.hpp"
#include "pu/infra/platform.hpp"
#include "pu/error.hpp"
#include "pu/json.hpp"

#include <boost/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <mutex>
#include <sstream>

namespace pu {

namespace {

std::string SafeString(const boost::json::value& j, const char* key) {
  return (json::HasKey(j, key) && j.at(key).is_string())
             ? boost::json::value_to<std::string>(j.at(key))
             : "";
}

boost::json::value BuildMessagesJson(const std::vector<ChatMessage>& history) {
  boost::json::array messages;
  for (const auto& msg : history) {
    std::string role = msg.role;
    if (role == "tool_result") role = "tool";

    boost::json::value j = {
      {"role", role}
    };

    bool has_tool_calls = !msg.tool_calls_json.empty();

    if (has_tool_calls) {
      j.as_object()["content"] = boost::json::value();
    } else {
      j.as_object()["content"] = msg.content.empty() ? "" : msg.content;
    }

    if (role == "assistant" && !msg.reasoning_content.empty()) {
      j.as_object()["reasoning_content"] = msg.reasoning_content;
    }

    if (role == "tool") {
      j.as_object()["tool_call_id"] = msg.tool_call_id;
    }

    if (has_tool_calls) {
      try {
        auto tool_calls = boost::json::parse(msg.tool_calls_json);
        for (auto& tc : tool_calls.as_array()) {
          if (json::HasKey(tc, "function") &&
              json::HasKey(tc.at("function"), "arguments")) {
            auto& args = tc.at("function").at("arguments");
            if (args.is_object() || args.is_array()) {
              args = boost::json::serialize(args);
            }
          }
        }
        j.as_object()["tool_calls"] = tool_calls;
      } catch (const std::exception&) {
        // ignore malformed tool_calls_json
      }
    }

    messages.push_back(j);
  }
  return messages;
}

}  // namespace

OpenAIProvider::OpenAIProvider(const Config& config,
                               std::unique_ptr<pu::http::HttpClient> http)
    : config_(config), http_(std::move(http)),
      host_(config_.host), api_key_(config_.api_key) {}

void OpenAIProvider::ResetAccumulators() {
  pending_tools_.clear();
  current_reasoning_content_.clear();
}

std::string OpenAIProvider::BuildRequest(const std::vector<ChatMessage>& history) const {
  boost::json::value req = {
    {"model", config_.model},
    {"stream", true},
    {"temperature", config_.temperature},
    {"max_tokens", config_.max_tokens},
  };

  if (!config_.enable_thinking) {
    boost::json::value extra_body = {{"thinking", {{"type", "disabled"}}}};
    req.as_object()["extra_body"] = extra_body;
  }

  auto messages = history;
  if (config_.system_prompt && std::none_of(history.begin(), history.end(), [](const ChatMessage& m) {
        return m.role == "system";
      })) {
    ChatMessage sys;
    sys.role = "system";
    sys.content = *config_.system_prompt;
    messages.insert(messages.begin(), std::move(sys));
  }

  req.as_object()["messages"] = BuildMessagesJson(messages);
  return boost::json::serialize(req);
}

std::string OpenAIProvider::BuildRequestWithTools(
    const std::vector<ChatMessage>& history,
    const std::vector<ToolDefinition>& tools) const {
  boost::json::value req = {
    {"model", config_.model},
    {"stream", true},
    {"temperature", config_.temperature},
    {"max_tokens", config_.max_tokens},
  };

  if (!config_.enable_thinking) {
    boost::json::value extra_body = {{"thinking", {{"type", "disabled"}}}};
    req.as_object()["extra_body"] = extra_body;
  }

  auto messages = history;
  if (config_.system_prompt && std::none_of(history.begin(), history.end(), [](const ChatMessage& m) {
        return m.role == "system";
      })) {
    ChatMessage sys;
    sys.role = "system";
    sys.content = *config_.system_prompt;
    messages.insert(messages.begin(), std::move(sys));
  }

  req.as_object()["messages"] = BuildMessagesJson(messages);

  boost::json::array tools_json;
  for (const auto& tool : tools) {
    boost::json::value params_json;
    try {
      params_json = boost::json::parse(tool.parameters_schema);
    } catch (const std::exception&) {
      params_json = boost::json::object{};
    }

    boost::json::value function_obj = {
      {"name", tool.name},
      {"description", tool.description},
      {"parameters", params_json}
    };

    tools_json.push_back(
        boost::json::value{{"type", "function"}, {"function", function_obj}});
  }
  req.as_object()["tools"] = tools_json;
  return boost::json::serialize(req);
}

void OpenAIProvider::HandleJsonToken(const boost::json::value& j,
                                     std::function<void(const std::string&)>& content_cb,
                                     std::function<void(const ToolCall&)>& tool_cb) {
  bool is_final = false;
  if (json::HasKey(j, "done") && boost::json::value_to<bool>(j.at("done"))) is_final = true;

  if (json::HasKey(j, "choices") && j.at("choices").is_array() &&
      !j.at("choices").as_array().empty()) {
    const boost::json::value delta =
        json::ValueOrDefault<boost::json::value>(j.at("choices").at(0), "delta",
                                                 boost::json::object{});
    if (delta.is_object()) {
      auto content = SafeString(delta, "content");
      if (!content.empty() && content_cb) content_cb(content);

      if (json::HasKey(delta, "reasoning_content") &&
          delta.at("reasoning_content").is_string()) {
        current_reasoning_content_ +=
            boost::json::value_to<std::string>(delta.at("reasoning_content"));
      }

      if (json::HasKey(delta, "tool_calls") && delta.at("tool_calls").is_array()) {
        for (const auto& tc : delta.at("tool_calls").as_array()) {
          if (!tc.is_object()) continue;
          int idx = json::ValueOrDefault<int>(tc, "index", -1);
          if (idx < 0) continue;
          auto& acc = pending_tools_[idx];
          auto id = SafeString(tc, "id");
          if (!id.empty()) acc.id = id;
          if (json::HasKey(tc, "function") && tc.at("function").is_object()) {
            auto name = SafeString(tc.at("function"), "name");
            if (!name.empty()) acc.name = name;
            acc.arguments += SafeString(tc.at("function"), "arguments");
          }
        }
      }
    }
  }

  if (json::HasKey(j, "usage") && j.at("usage").is_object()) {
    const auto& usage = j.at("usage");
    spdlog::trace("OpenAI usage: prompt_tokens={}, completion_tokens={}, total_tokens={}",
                  json::ValueOrDefault<int>(usage, "prompt_tokens", 0),
                  json::ValueOrDefault<int>(usage, "completion_tokens", 0),
                  json::ValueOrDefault<int>(usage, "total_tokens", 0));
  }

  if (is_final) {
    for (auto& [idx, acc] : pending_tools_) {
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
      if (tool_cb) tool_cb(call);
    }
    pending_tools_.clear();
  }
}

ChatResult OpenAIProvider::Chat(
    const std::vector<ChatMessage>& history,
    const std::vector<ToolDefinition>& tools,
    std::function<void(const std::string&)> content_callback,
    std::function<void(const ToolCall&)> tool_callback,
    CancelToken cancel_token) {
  ChatResult result;
  platform::ClearInterruptFlag();
  ResetAccumulators();

  std::string body;
  if (tools.empty()) {
    body = BuildRequest(history);
  } else {
    body = BuildRequestWithTools(history, tools);
  }

  spdlog::debug("OpenAI request body: {}", body);

  std::string url = host_ + "/chat/completions";
  std::vector<std::string> headers = {"Content-Type: application/json"};
  if (!api_key_.empty()) headers.push_back("Authorization: Bearer " + api_key_);

  std::ostringstream content_stream;
  std::vector<ToolCall> collected_calls;

  llm::StreamingJsonParser parser(
    [&](std::string_view line) {
      constexpr std::string_view kDataPrefix = "data: ";
      auto start = line.find_first_not_of(" \t");
      if (start == std::string_view::npos) return;
      std::string_view trimmed = line.substr(start);
      if (trimmed.substr(0, kDataPrefix.size()) != kDataPrefix) return;
      std::string_view data = trimmed.substr(kDataPrefix.size());
      if (data == "[DONE]") {
        boost::json::value done_obj = {{"done", true}};
        HandleJsonToken(done_obj, content_callback, tool_callback);
        return;
      }
      try {
        auto j = boost::json::parse(data);
        HandleJsonToken(j, content_callback, tool_callback);

        if (json::HasKey(j, "choices") && j.at("choices").is_array() &&
            !j.at("choices").as_array().empty()) {
          const boost::json::value delta =
              json::ValueOrDefault<boost::json::value>(j.at("choices").at(0), "delta",
                                                       boost::json::object{});
          if (delta.is_object()) {
            auto content = SafeString(delta, "content");
            content_stream << content;
          }
        }
      } catch (const boost::system::system_error& e) {
        // Skip lines with incomplete/invalid UTF-8 instead of failing the stream.
        spdlog::warn("Skipping invalid JSON line (UTF-8 error): {}", e.what());
      } catch (const std::exception&) {}
    },
    [](const std::string& msg) { throw HttpError("OpenAI streaming error: " + msg); }
  );

  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (platform::IsInterrupted()) return 0;
    if (cancel_token && cancel_token->load(std::memory_order_acquire)) return 0;
    return total;
  };

  http_->PostStream(url, body, headers, write_cb, cancel_token);

  result.content = content_stream.str();
  result.tool_calls = std::move(collected_calls);
  result.reasoning_content = current_reasoning_content_;
  return result;
}

}  // namespace pu
