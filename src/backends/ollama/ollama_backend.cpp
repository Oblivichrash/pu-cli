// SPDX-License-Identifier: GPL-3.0-only

#include "ollama_backend.hpp"
#include "sse_parser.hpp"
#include "pu/backend_helpers.hpp"
#include "platform/platform.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace pu::backends::ollama {

using json = nlohmann::json;

std::string OllamaBackend::BuildRequest(const std::vector<pu::backend::Message>& history) const {
  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["options"]["temperature"] = config_.temperature;

  auto messages_history = InjectSystemPrompt(history, config_.system_prompt);
  json messages = json::array();
  for (const auto& msg : messages_history) {
    std::string role;
    switch (msg.role) {
      case pu::backend::Message::Role::kUser: role = "user"; break;
      case pu::backend::Message::Role::kAssistant: role = "assistant"; break;
      case pu::backend::Message::Role::kSystem: role = "system"; break;
      case pu::backend::Message::Role::kTool: role = "tool"; break;
    }
    if (!role.empty()) {
      messages.push_back({{"role", role}, {"content", msg.content}});
    }
  }
  req["messages"] = messages;
  return req.dump();
}

OllamaBackend::OllamaBackend(Config config,
                             std::unique_ptr<pu::http::HttpClient> http)
    : Backend(std::move(config)), host_(std::move(config.host)),
      api_key_(std::move(config.api_key)), http_(std::move(http)) {}

void OllamaBackend::Chat(const std::vector<pu::backend::Message>& history,
                         pu::backend::ChatCallback cb) {
  pu::platform::ClearInterruptFlag();

  std::string body = BuildRequest(history);
  std::string url = host_ + "/api/chat";

  std::vector<std::string> headers;
  headers.push_back("Content-Type: application/json");
  if (!api_key_.empty()) {
    headers.push_back("Authorization: Bearer " + api_key_);
  }

  std::string line_buffer;
  std::string accumulated_content;
  std::string accumulated_reasoning;

  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    line_buffer.append(ptr, total);
    size_t pos = 0;
    while ((pos = line_buffer.find('\n')) != std::string::npos) {
      std::string line = line_buffer.substr(0, pos);
      line_buffer.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();

      auto token_opt = internal::ParseSseLine(line);
      if (!token_opt) continue;

      const auto& token = *token_opt;
      if (token.done) {
        cb(pu::backend::TokenType::kContent, "", true);
        return total;
      }

      auto extract_delta = [](std::string_view new_text, std::string& accumulated) -> std::string_view {
        if (new_text.empty()) return new_text;
        if (new_text.size() < accumulated.size()) {
          accumulated.clear();
        }
        if (accumulated.empty() ||
            new_text.compare(0, accumulated.size(), accumulated) != 0) {
          accumulated = new_text;
          return new_text;
        }
        std::string_view delta = new_text.substr(accumulated.size());
        accumulated = new_text;
        return delta;
      };

      if (!token.reasoning.empty()) {
        std::string_view delta = extract_delta(token.reasoning, accumulated_reasoning);
        if (!delta.empty()) {
          cb(pu::backend::TokenType::kReasoning, delta, false);
        }
      }

      if (!token.content.empty()) {
        std::string_view delta = extract_delta(token.content, accumulated_content);
        if (!delta.empty()) {
          cb(pu::backend::TokenType::kContent, delta, false);
        }
      }
    }
    return total;
  };

  http_->PostStream(url, body, headers, write_cb);
}

void OllamaBackend::Chat(const std::vector<pu::backend::Message>& history,
                         const std::vector<pu::backend::ToolDefinition>& tools,
                         pu::backend::ChatCallback content_cb,
                         pu::backend::ToolCallback tool_cb) {
  pu::platform::ClearInterruptFlag();

  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["options"]["temperature"] = config_.temperature;

  auto messages_history = InjectSystemPrompt(history, config_.system_prompt);
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
    if (role == "tool") {
      m["tool_name"] = msg.tool_name;
    }
    if (!msg.tool_calls.empty()) {
      json tcs = json::array();
      for (const auto& tc : msg.tool_calls) {
        json func = {{"name", tc.name}};
        if (!tc.arguments.empty()) {
          try {
            func["arguments"] = json::parse(tc.arguments);
          } catch (...) {
            func["arguments"] = tc.arguments;
          }
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
    t["function"]["parameters"] = json::parse(tool.parameters.raw_schema);
    tools_json.push_back(t);
  }
  req["tools"] = tools_json;

  std::string body = req.dump();
  std::string url = host_ + "/api/chat";

  std::vector<std::string> headers;
  headers.push_back("Content-Type: application/json");
  if (!api_key_.empty()) {
    headers.push_back("Authorization: Bearer " + api_key_);
  }

  std::string line_buffer;

  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    line_buffer.append(ptr, total);
    size_t pos = 0;
    while ((pos = line_buffer.find('\n')) != std::string::npos) {
      std::string line = line_buffer.substr(0, pos);
      line_buffer.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();

      auto token_opt = internal::ParseSseLine(line);
      if (!token_opt) continue;

      const auto& token = *token_opt;
      if (token.done) {
        content_cb(pu::backend::TokenType::kContent, "", true);
        return total;
      }

      if (!token.content.empty()) {
        content_cb(pu::backend::TokenType::kContent, token.content, false);
      }
      if (!token.reasoning.empty()) {
        content_cb(pu::backend::TokenType::kReasoning, token.reasoning, false);
      }
      for (const auto& call : token.tool_calls) {
        tool_cb(call);
      }
    }
    return total;
  };

  http_->PostStream(url, body, headers, write_cb);
}

}  // namespace pu::backends::ollama
