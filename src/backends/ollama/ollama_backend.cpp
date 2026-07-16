// SPDX-License-Identifier: GPL-3.0-only
#include "backends/ollama/ollama_backend.hpp"
#include "pu/backend_helpers.hpp"
#include "pu/error_codes.hpp"
#include "platform/platform.hpp"
#include "backends/common/streaming_json_parser.hpp"
#include <nlohmann/json.hpp>
#include <iostream>

namespace pu::backends::ollama {

using json = nlohmann::json;

std::string OllamaBackend::BuildRequest(const std::vector<pu::backend::Message>& history) const {
  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["options"]["temperature"] = config_.temperature;

  auto messages_history = pu::backend::InjectSystemPrompt(history, config_.system_prompt);
  json messages = json::array();
  for (const auto& msg : messages_history) {
    std::string role;
    switch (msg.role) {
      case pu::backend::Message::Role::kUser: role = "user"; break;
      case pu::backend::Message::Role::kAssistant: role = "assistant"; break;
      case pu::backend::Message::Role::kSystem: role = "system"; break;
      case pu::backend::Message::Role::kTool: role = "tool"; break;
    }
    if (!role.empty()) messages.push_back({{"role", role}, {"content", msg.content}});
  }
  req["messages"] = messages;
  return req.dump();
}

std::string OllamaBackend::BuildRequestWithTools(
    const std::vector<pu::backend::Message>& history,
    const std::vector<pu::backend::ToolDefinition>& tools) const {
  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["options"]["temperature"] = config_.temperature;

  auto messages_history = pu::backend::InjectSystemPrompt(history, config_.system_prompt);
  json messages = json::array();
  for (const auto& msg : messages_history) {
    std::string role;
    switch (msg.role) {
      case pu::backend::Message::Role::kSystem: role = "system"; break;
      case pu::backend::Message::Role::kUser: role = "user"; break;
      case pu::backend::Message::Role::kAssistant: role = "assistant"; break;
      case pu::backend::Message::Role::kTool: role = "tool"; break;
    }
    json m = {{"role", role}, {"content", msg.content}};
    if (role == "tool") m["tool_name"] = msg.tool_name;
    if (!msg.tool_calls.empty()) {
      json tcs = json::array();
      for (const auto& tc : msg.tool_calls) {
        json func = {{"name", tc.name}};
        if (!tc.arguments.empty()) {
          try { func["arguments"] = json::parse(tc.arguments); }
          catch (...) { func["arguments"] = tc.arguments; }
        }
        tcs.push_back({{"function", func}});
      }
      m["tool_calls"] = tcs;
    }
    messages.push_back(m);
  }
  req["messages"] = messages;

  json tools_json = json::array();
  for (const auto& tool : tools) {
    json t;
    t["type"] = "function";
    t["function"]["name"] = tool.name;
    t["function"]["description"] = tool.description;
    try {
      t["function"]["parameters"] = json::parse(tool.parameters.raw_schema);
    } catch (const std::exception& e) {
      std::cerr << "[OllamaBackend] Failed to parse schema for tool '" << tool.name
                << "': " << e.what() << std::endl;
      continue;
    }
    tools_json.push_back(t);
  }
  req["tools"] = tools_json;
  return req.dump();
}

OllamaBackend::OllamaBackend(Config config, std::unique_ptr<pu::http::HttpClient> http,
                             std::unique_ptr<ITokenAdapter> adapter)
    : Backend(std::move(config)), host_(std::move(config.host)),
      api_key_(std::move(config.api_key)), http_(std::move(http)),
      adapter_(std::move(adapter)) {}

void OllamaBackend::Chat(const std::vector<pu::backend::Message>& history,
                         pu::backend::ChatCallback cb, std::error_code& ec) {
  pu::platform::ClearInterruptFlag();
  adapter_->Reset();
  auto body = BuildRequest(history);
  std::string url = host_ + "/api/chat";

  std::vector<std::string> headers = {"Content-Type: application/json"};
  if (!api_key_.empty()) headers.push_back("Authorization: Bearer " + api_key_);

  StreamingJsonParser parser(
    [this, cb](std::string_view line) {
      try {
        auto j = json::parse(line);
        adapter_->HandleJson(j, cb, [](const backend::ToolCall&) {});
      } catch (const std::exception&) {}
    },
    [&ec](const std::string& msg) { if (!ec) ec = pu::HttpErrc::http_error; }
  );
  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (pu::platform::IsInterrupted()) return 0;
    return total;
  };
  http_->PostStream(url, body, headers, write_cb, ec);
}

void OllamaBackend::Chat(const std::vector<pu::backend::Message>& history,
                         const std::vector<pu::backend::ToolDefinition>& tools,
                         pu::backend::ChatCallback content_cb,
                         pu::backend::ToolCallback tool_cb, std::error_code& ec) {
  pu::platform::ClearInterruptFlag();
  adapter_->Reset();
  auto body = BuildRequestWithTools(history, tools);
  std::string url = host_ + "/api/chat";

  std::vector<std::string> headers = {"Content-Type: application/json"};
  if (!api_key_.empty()) headers.push_back("Authorization: Bearer " + api_key_);

  StreamingJsonParser parser(
    [this, content_cb, tool_cb](std::string_view line) {
      try {
        auto j = json::parse(line);
        adapter_->HandleJson(j, content_cb, tool_cb);
      } catch (const std::exception&) {}
    },
    [&ec](const std::string& msg) { if (!ec) ec = pu::HttpErrc::http_error; }
  );
  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (pu::platform::IsInterrupted()) return 0;
    return total;
  };
  http_->PostStream(url, body, headers, write_cb, ec);
}

}  // namespace pu::backends::ollama
