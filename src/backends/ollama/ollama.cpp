// SPDX-License-Identifier: GPL-3.0-only
#include "backends/request_builder.hpp"
#include "ollama.hpp"
#include "core/system.hpp"
#include "core/error.hpp"
#include "backends/streaming_json_parser.hpp"
#include <nlohmann/json.hpp>
#include <iostream>

namespace pu::backends::ollama {

using json = nlohmann::json;

static void HandleJsonChunk(const json& j,
                            pu::backend::ChatCallback content_cb,
                            pu::backend::ToolCallback tool_cb) {
  if (j.contains("message")) {
    const auto& msg = j["message"];
    if (msg.contains("content") && msg["content"].is_string())
      content_cb(pu::backend::TokenType::kContent, msg["content"].get<std::string>(), false);
    if (msg.contains("thinking") && msg["thinking"].is_string())
      content_cb(pu::backend::TokenType::kReasoning, msg["thinking"].get<std::string>(), false);
    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
      for (const auto& tc : msg["tool_calls"]) {
        pu::backend::ToolCall call;
        if (tc.contains("id")) call.id = tc["id"].get<std::string>();
        if (tc.contains("function")) {
          call.name = tc["function"].value("name", "");
          if (tc["function"].contains("arguments")) {
            const auto& args = tc["function"]["arguments"];
            if (args.is_string()) call.arguments = args.get<std::string>();
            else if (args.is_object() || args.is_array()) call.arguments = args.dump();
          }
        }
        tool_cb(call);
      }
    }
  }
  if (j.contains("done") && j["done"].get<bool>()) {
    content_cb(pu::backend::TokenType::kContent, "", true);
  }
}

OllamaBackend::OllamaBackend(Config config, std::unique_ptr<pu::http::HttpClient> http)
    : Backend(std::move(config)), host_(std::move(config.host)),
      api_key_(std::move(config.api_key)), http_(std::move(http)) {}

void OllamaBackend::Chat(const std::vector<pu::backend::Message>& history,
                         pu::backend::ChatCallback cb) {
  pu::platform::ClearInterruptFlag();
  auto req = RequestBuilder::BuildChatRequest(
      pu::backends::BackendFlavor::kOllama,
      config_.model,
      config_.temperature,
      history,
      config_.system_prompt
  );
  std::string body = req.dump();
  std::string url = host_ + "/api/chat";

  std::vector<std::string> headers = {"Content-Type: application/json"};
  if (!api_key_.empty()) headers.push_back("Authorization: Bearer " + api_key_);

  StreamingJsonParser parser(
    [cb](std::string_view line) {
      try {
        auto j = json::parse(line);
        HandleJsonChunk(j, cb, [](const pu::backend::ToolCall&) {});
      } catch (const std::exception&) {}
    },
    [](const std::string& msg) { throw pu::HttpError(msg); }
  );
  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (pu::platform::IsInterrupted()) return 0;
    return total;
  };
  http_->PostStream(url, body, headers, write_cb);
}

void OllamaBackend::Chat(const std::vector<pu::backend::Message>& history,
                         const std::vector<pu::backend::ToolDefinition>& tools,
                         pu::backend::ChatCallback content_cb,
                         pu::backend::ToolCallback tool_cb) {
  pu::platform::ClearInterruptFlag();
  auto req = RequestBuilder::BuildChatRequestWithTools(
      pu::backends::BackendFlavor::kOllama,
      config_.model,
      config_.temperature,
      history,
      config_.system_prompt,
      tools
  );
  std::string body = req.dump();
  std::string url = host_ + "/api/chat";

  std::vector<std::string> headers = {"Content-Type: application/json"};
  if (!api_key_.empty()) headers.push_back("Authorization: Bearer " + api_key_);

  StreamingJsonParser parser(
    [content_cb, tool_cb](std::string_view line) {
      try {
        auto j = json::parse(line);
        HandleJsonChunk(j, content_cb, tool_cb);
      } catch (const std::exception&) {}
    },
    [](const std::string& msg) { throw pu::HttpError(msg); }
  );
  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (pu::platform::IsInterrupted()) return 0;
    return total;
  };
  http_->PostStream(url, body, headers, write_cb);
}

}  // namespace pu::backends::ollama
