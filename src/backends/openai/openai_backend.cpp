// SPDX-License-Identifier: GPL-3.0-only

#include "openai_backend.hpp"
#include "sse_parser.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace pu::backends::openai {

using json = nlohmann::json;

std::string OpenAIBackend::BuildRequest(const std::vector<pu::backend::Message>& history) const {
  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["temperature"] = config_.temperature;

  auto messages_history = BuildMessagesWithSystemPrompt(history);
  json messages = json::array();
  for (const auto& msg : messages_history) {
    std::string role;
    switch (msg.role) {
      case pu::backend::Message::Role::kSystem: role = "system"; break;
      case pu::backend::Message::Role::kUser: role = "user"; break;
      case pu::backend::Message::Role::kAssistant: role = "assistant"; break;
      case pu::backend::Message::Role::kTool: role = "tool"; break;
    }
    messages.push_back({{"role", role}, {"content", msg.content}});
  }
  req["messages"] = messages;
  return req.dump();
}

OpenAIBackend::OpenAIBackend(const Config& config,
                             std::unique_ptr<pu::http::HttpClient> http)
    : Backend(config),
      http_(std::move(http)),
      host_(config.host),
      api_key_(config.api_key) {}

void OpenAIBackend::Chat(const std::vector<pu::backend::Message>& history,
                         pu::backend::ChatCallback cb) {
  std::string body = BuildRequest(history);
  std::string url = host_ + "/v1/chat/completions";

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

void OpenAIBackend::Chat([[maybe_unused]] const std::vector<pu::backend::Message>& history,
                         [[maybe_unused]] const std::vector<pu::backend::ToolDefinition>& tools,
                         [[maybe_unused]] pu::backend::ChatCallback content_cb,
                         [[maybe_unused]] pu::backend::ToolCallback tool_cb) {
  throw std::runtime_error("OpenAIBackend does not support tool calling yet");
}

}  // namespace pu::backends::openai
