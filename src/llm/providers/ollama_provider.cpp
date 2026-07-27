// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/providers/ollama_provider.hpp"

#include "pu/llm/common/streaming_json_parser.hpp"
#include "infra/platform.hpp"
#include "pu/error.hpp"

#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>

namespace pu {

using json = nlohmann::json;

OllamaProvider::OllamaProvider(Config config, std::unique_ptr<pu::http::HttpClient> http)
    : config_(std::move(config)), host_(config_.host),
      api_key_(std::move(config_.api_key)), http_(std::move(http)) {}

std::string OllamaProvider::RoleToString(const std::string& role) const {
  if (role == "user") return "user";
  if (role == "assistant" || role == "tool_result") return "assistant";
  if (role == "system") return "system";
  if (role == "tool") return "tool";
  return "user";
}

ChatMessage OllamaProvider::ToChatMessage(const json& msg) const {
  ChatMessage cm;
  cm.role = msg.value("role", "user");
  cm.content = msg.value("content", "");
  cm.tool_name = msg.value("tool_name", "");
  if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
    cm.tool_calls_json = msg["tool_calls"].dump();
  }
  return cm;
}

std::string OllamaProvider::BuildRequest(const std::vector<ChatMessage>& history) const {
  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["options"]["temperature"] = config_.temperature;

  auto messages = history;
  if (config_.system_prompt && std::none_of(history.begin(), history.end(), [](const ChatMessage& m) {
        return m.role == "system";
      })) {
    ChatMessage sys;
    sys.role = "system";
    sys.content = *config_.system_prompt;
    messages.insert(messages.begin(), std::move(sys));
  }

  json msgs = json::array();
  for (const auto& msg : messages) {
    json m = {{"role", RoleToString(msg.role)}, {"content", msg.content}};
    if (msg.role == "tool") {
      m["tool_name"] = msg.tool_name;
    }
    if (!msg.tool_calls_json.empty()) {
      try {
        m["tool_calls"] = json::parse(msg.tool_calls_json);
      } catch (const std::exception&) {}
    }
    msgs.push_back(m);
  }
  req["messages"] = msgs;
  return req.dump();
}

std::string OllamaProvider::BuildRequestWithTools(
    const std::vector<ChatMessage>& history,
    const std::vector<ToolDefinition>& tools) const {
  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["options"]["temperature"] = config_.temperature;

  auto messages = history;
  if (config_.system_prompt && std::none_of(history.begin(), history.end(), [](const ChatMessage& m) {
        return m.role == "system";
      })) {
    ChatMessage sys;
    sys.role = "system";
    sys.content = *config_.system_prompt;
    messages.insert(messages.begin(), std::move(sys));
  }

  json msgs = json::array();
  for (const auto& msg : messages) {
    json m = {{"role", RoleToString(msg.role)}, {"content", msg.content}};
    if (msg.role == "tool") {
      m["tool_name"] = msg.tool_name;
    }
    if (!msg.tool_calls_json.empty()) {
      try {
        auto tool_calls = json::parse(msg.tool_calls_json);
        // Convert from our stored format to Ollama format
        json tcs = json::array();
        for (const auto& tc : tool_calls) {
          json func = {{"name", tc.value("name", "")}};
          if (tc.contains("arguments")) {
            const auto& args = tc["arguments"];
            if (args.is_string()) {
              try { func["arguments"] = json::parse(args.get<std::string>()); }
              catch (...) { func["arguments"] = args.get<std::string>(); }
            } else if (args.is_object() || args.is_array()) {
              func["arguments"] = args;
            }
          }
          json tc_entry;
          if (tc.contains("id")) tc_entry["id"] = tc["id"];
          tc_entry["function"] = func;
          tcs.push_back(tc_entry);
        }
        m["tool_calls"] = tcs;
      } catch (const std::exception&) {}
    }
    msgs.push_back(m);
  }
  req["messages"] = msgs;

  json tools_json = json::array();
  for (const auto& tool : tools) {
    json t;
    t["type"] = "function";
    t["function"]["name"] = tool.name;
    t["function"]["description"] = tool.description;
    try {
      t["function"]["parameters"] = json::parse(tool.parameters_schema);
    } catch (const std::exception& e) {
      std::cerr << "[OllamaProvider] Failed to parse schema for tool '" << tool.name
                << "': " << e.what() << '\n';
      continue;
    }
    tools_json.push_back(t);
  }
  req["tools"] = tools_json;
  return req.dump();
}

void OllamaProvider::HandleJsonToken(const json& j,
                                     std::function<void(const std::string&)>& content_cb,
                                     std::function<void(const ToolCall&)>& tool_cb) {
  if (j.contains("message")) {
    const auto& msg = j["message"];
    if (msg.contains("content") && msg["content"].is_string())
      if (content_cb) content_cb(msg["content"].get<std::string>());
    if (msg.contains("thinking") && msg["thinking"].is_string())
      if (content_cb) content_cb(msg["thinking"].get<std::string>());
    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
      for (const auto& tc : msg["tool_calls"]) {
        ToolCall call;
        if (tc.contains("function")) {
          call.name = tc["function"].value("name", "");
          if (tc["function"].contains("arguments")) {
            const auto& args = tc["function"]["arguments"];
            if (args.is_string()) {
              try { call.arguments = json::parse(args.get<std::string>()); }
              catch (...) { call.arguments = args.get<std::string>(); }
            } else if (args.is_object() || args.is_array()) {
              call.arguments = args;
            }
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
    std::function<void(const ToolCall&)> tool_callback) {
  ChatResult result;
  pu::platform::ClearInterruptFlag();

  std::string body;
  if (tools.empty()) {
    body = BuildRequest(history);
  } else {
    body = BuildRequestWithTools(history, tools);
  }

  std::string url = host_ + "/api/chat";
  std::vector<std::string> headers = {"Content-Type: application/json"};
  if (!api_key_.empty()) headers.push_back("Authorization: Bearer " + api_key_);

  std::ostringstream content_stream;
  std::vector<ToolCall> collected_calls;

  llm::StreamingJsonParser parser(
    [&](std::string_view line) {
      try {
        auto j = json::parse(line);
        HandleJsonToken(j, content_callback, tool_callback);

        // Also collect locally for the result
        if (j.contains("message")) {
          const auto& msg = j["message"];
          if (msg.contains("content") && msg["content"].is_string()) {
            content_stream << msg["content"].get<std::string>();
          }
          if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
            for (const auto& tc : msg["tool_calls"]) {
              ToolCall call;
              if (tc.contains("function")) {
                call.name = tc["function"].value("name", "");
                if (tc["function"].contains("arguments")) {
                  const auto& args = tc["function"]["arguments"];
                  if (args.is_string()) {
                    try { call.arguments = json::parse(args.get<std::string>()); }
                    catch (...) { call.arguments = args.get<std::string>(); }
                  } else if (args.is_object() || args.is_array()) {
                    call.arguments = args;
                  }
                }
              }
              collected_calls.push_back(std::move(call));
            }
          }
        }
      } catch (const std::exception&) {}
    },
    [](const std::string& msg) { throw HttpError("Streaming error: " + msg); }
  );

  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (pu::platform::IsInterrupted()) return 0;
    return total;
  };

  http_->PostStream(url, body, headers, write_cb);

  result.content = content_stream.str();
  result.tool_calls = std::move(collected_calls);
  return result;
}

}  // namespace pu
