// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.

#include "openai_backend.hpp"
#include "sse_parser.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace pu::backends::openai {

using json = nlohmann::json;

// ============================================================================
// Request building
// ============================================================================
std::string OpenAIBackend::BuildRequest(const std::vector<pu::backend::Message>& history) const {
  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["temperature"] = config_.temperature;

  json messages = json::array();
  if (config_.system_prompt) {
    messages.push_back({{"role", "system"}, {"content", *config_.system_prompt}});
  }
  for (const auto& msg : history) {
    std::string role;
    switch (msg.role) {
      case pu::backend::Message::Role::kUser: role = "user"; break;
      case pu::backend::Message::Role::kAssistant: role = "assistant"; break;
      case pu::backend::Message::Role::kSystem: role = "system"; break;
    }
    messages.push_back({{"role", role}, {"content", msg.content}});
  }
  req["messages"] = messages;
  return req.dump();
}

// ============================================================================
// Public API
// ============================================================================
OpenAIBackend::OpenAIBackend(const Config& config,
                             std::unique_ptr<pu::http::HttpClient> http)
    : Backend(config),                // base class copies Config
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
  std::string current_content;

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
      } else if (!token.content.empty()) {
        current_content += token.content;
        cb(pu::backend::TokenType::kContent, token.content, false);
      }
    }
    return total;
  };

  http_->PostStream(url, body, headers, write_cb);
}

}  // namespace pu::backends::openai
