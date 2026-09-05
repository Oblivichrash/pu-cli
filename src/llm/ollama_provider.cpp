// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/ollama_provider.hpp"

#include "pu/llm/streaming_json_parser.hpp"
#include "pu/infra/platform.hpp"
#include "pu/error.hpp"
#include "pu/json.hpp"

#include <boost/json.hpp>
#include <spdlog/spdlog.h>
#include <sstream>

namespace pu {

OllamaProvider::OllamaProvider(Config config, std::unique_ptr<pu::http::HttpClient> http)
    : config_(std::move(config)), host_(config_.host),
      api_key_(std::move(config_.api_key)), http_(std::move(http)) {}

std::string OllamaProvider::RoleToString(const std::string& role) const {
  if (role == "user") return "user";
  if (role == "assistant") return "assistant";
  if (role == "system") return "system";
  if (role == "tool") return "tool";
  return "user";
}

std::string OllamaProvider::BuildRequest(const std::vector<ChatMessage>& history) const {
  boost::json::value req = {
    {"model", config_.model},
    {"stream", true},
    {"options", {{"temperature", config_.temperature}}},
    {"keep_alive", config_.keep_alive},
  };

  auto messages = history;
  if (config_.system_prompt && std::none_of(history.begin(), history.end(), [](const ChatMessage& m) {
        return m.role == "system";
      })) {
    ChatMessage sys;
    sys.role = "system";
    sys.content = *config_.system_prompt;
    messages.insert(messages.begin(), std::move(sys));
  }

  boost::json::array msgs;
  for (const auto& msg : messages) {
    boost::json::value m = {{"role", RoleToString(msg.role)}, {"content", msg.content}};
    if (msg.role == "tool") {
      m.as_object()["tool_name"] = msg.tool_name;
      if (!msg.tool_call_id.empty()) {
        m.as_object()["tool_call_id"] = msg.tool_call_id;
      }
    }
    if (!msg.tool_calls_json.empty()) {
      try {
        m.as_object()["tool_calls"] = boost::json::parse(msg.tool_calls_json);
      } catch (const std::exception&) {}
    }
    msgs.push_back(m);
  }
  req.as_object()["messages"] = msgs;
  return boost::json::serialize(req);
}

std::string OllamaProvider::BuildRequestWithTools(
    const std::vector<ChatMessage>& history,
    const std::vector<ToolDefinition>& tools) const {
  boost::json::value req = {
    {"model", config_.model},
    {"stream", true},
    {"options", {{"temperature", config_.temperature}}},
    {"keep_alive", config_.keep_alive},
  };

  auto messages = history;
  if (config_.system_prompt && std::none_of(history.begin(), history.end(), [](const ChatMessage& m) {
        return m.role == "system";
      })) {
    ChatMessage sys;
    sys.role = "system";
    sys.content = *config_.system_prompt;
    messages.insert(messages.begin(), std::move(sys));
  }

  boost::json::array msgs;
  for (const auto& msg : messages) {
    boost::json::value m = {{"role", RoleToString(msg.role)}, {"content", msg.content}};
    if (msg.role == "tool") {
      m.as_object()["tool_name"] = msg.tool_name;
      if (!msg.tool_call_id.empty()) {
        m.as_object()["tool_call_id"] = msg.tool_call_id;
      }
    }
    if (!msg.tool_calls_json.empty()) {
      try {
        auto tool_calls = boost::json::parse(msg.tool_calls_json);
        boost::json::array tcs;
        for (const auto& tc : tool_calls.as_array()) {
          boost::json::value func = {
            {"name", json::ValueOrDefault<std::string>(tc, "name", "")}
          };
          if (json::HasKey(tc, "arguments")) {
            const auto& args = tc.at("arguments");
            if (args.is_string()) {
              try {
                func.as_object()["arguments"] =
                    boost::json::parse(boost::json::value_to<std::string>(args));
              } catch (...) {
                func.as_object()["arguments"] = args;
              }
            } else if (args.is_object() || args.is_array()) {
              func.as_object()["arguments"] = args;
            }
          }
          boost::json::value tc_entry = boost::json::object{};
          if (json::HasKey(tc, "id")) tc_entry.as_object()["id"] = tc.at("id");
          tc_entry.as_object()["function"] = func;
          tcs.push_back(tc_entry);
        }
        m.as_object()["tool_calls"] = tcs;
      } catch (const std::exception&) {}
    }
    msgs.push_back(m);
  }
  req.as_object()["messages"] = msgs;

  boost::json::array tools_json;
  for (const auto& tool : tools) {
    boost::json::value t = {{"type", "function"}};
    try {
      t.as_object()["function"] = {
        {"name", tool.name},
        {"description", tool.description},
        {"parameters", boost::json::parse(tool.parameters_schema)},
      };
    } catch (const std::exception& e) {
      spdlog::error("[OllamaProvider] Failed to parse schema for tool '{}': {}", tool.name, e.what());
      continue;
    }
    tools_json.push_back(t);
  }
  req.as_object()["tools"] = tools_json;
  return boost::json::serialize(req);
}

void OllamaProvider::HandleJsonToken(const boost::json::value& j,
                                     std::function<void(const std::string&)>& content_cb,
                                     std::function<void(const ToolCall&)>& tool_cb) {
  if (json::HasKey(j, "message")) {
    const auto& msg = j.at("message");
    if (json::HasKey(msg, "content") && msg.at("content").is_string())
      if (content_cb) content_cb(boost::json::value_to<std::string>(msg.at("content")));

    if (json::HasKey(msg, "tool_calls") && msg.at("tool_calls").is_array()) {
      for (const auto& tc : msg.at("tool_calls").as_array()) {
        if (!json::HasKey(tc, "function")) continue;
        std::string tool_name =
            json::ValueOrDefault<std::string>(tc.at("function"), "name", "");
        if (tool_name.empty()) continue;

        ToolCall call;
        if (json::HasKey(tc, "id") && tc.at("id").is_string()) {
          call.id = boost::json::value_to<std::string>(tc.at("id"));
        }
        call.name = tool_name;
        if (json::HasKey(tc.at("function"), "arguments")) {
          const auto& args = tc.at("function").at("arguments");
          if (args.is_string()) {
            try {
              call.arguments = boost::json::parse(boost::json::value_to<std::string>(args));
            } catch (...) {
              call.arguments = args;
            }
          } else if (args.is_object() || args.is_array()) {
            call.arguments = args;
          }
        }
        if (tool_cb) tool_cb(call);
      }
    }
  }
}

ChatResult OllamaProvider::Chat(
    const std::vector<ChatMessage>& history,
    const std::vector<ToolDefinition>& tools,
    std::function<void(const std::string&)> content_callback,
    std::function<void(const ToolCall&)> tool_callback,
    CancelToken cancel_token) {
  ChatResult result;
  platform::ClearInterruptFlag();

  std::string body;
  if (tools.empty()) {
    body = BuildRequest(history);
  } else {
    body = BuildRequestWithTools(history, tools);
  }

  spdlog::debug("Ollama request body: {}", body);

  std::string url = host_ + "/api/chat";
  std::vector<std::string> headers = {"Content-Type: application/json"};
  if (!api_key_.empty()) headers.push_back("Authorization: Bearer " + api_key_);

  std::ostringstream content_stream;

  llm::StreamingJsonParser parser(
    [&](std::string_view line) {
      try {
        auto j = boost::json::parse(line);
        HandleJsonToken(j, content_callback, tool_callback);

        if (json::HasKey(j, "message")) {
          const auto& msg = j.at("message");
          if (json::HasKey(msg, "content") && msg.at("content").is_string()) {
            content_stream << boost::json::value_to<std::string>(msg.at("content"));
          }
        }
      } catch (const boost::system::system_error& e) {
        // Skip lines with incomplete/invalid UTF-8 instead of failing the stream.
        spdlog::warn("Skipping invalid JSON line (UTF-8 error): {}", e.what());
      } catch (const std::exception&) {}
    },
    [](const std::string& msg) { throw HttpError("Streaming error: " + msg); }
  );

  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (platform::IsInterrupted()) return 0;
    if (cancel_token && cancel_token->load(std::memory_order_acquire)) return 0;
    return total;
  };

  http_->PostStream(url, body, headers, write_cb, cancel_token);

  result.content = content_stream.str();
  return result;
}

}  // namespace pu
