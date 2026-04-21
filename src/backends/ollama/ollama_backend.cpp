// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.

#include "ollama_backend.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace pu::backends::ollama {

using json = nlohmann::json;

// ============================================================================
// SSE parsing (pure function, testable)
// ============================================================================
std::optional<OllamaBackend::SseToken> OllamaBackend::ParseSseLine(std::string_view line) {
  if (line.empty()) return std::nullopt;
  try {
    json j = json::parse(line);
    SseToken token;
    if (j.contains("done") && j["done"].get<bool>()) {
      token.done = true;
      return token;
    }
    if (j.contains("message") && j["message"].contains("content")) {
      token.content = j["message"]["content"];
      token.done = false;
      return token;
    }
  } catch (const std::exception&) {
    throw std::runtime_error("JSON parse error in SSE line: " + std::string(line));
  }
  return std::nullopt;
}

// ============================================================================
// Request building
// ============================================================================
std::string OllamaBackend::BuildRequest(const std::vector<Message>& history) const {
  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["options"]["temperature"] = config_.temperature;

  json messages = json::array();
  if (config_.system_prompt) {
    messages.push_back({{"role", "system"}, {"content", *config_.system_prompt}});
  }
  for (const auto& msg : history) {
    std::string role;
    switch (msg.role) {
      case Message::Role::kUser: role = "user"; break;
      case Message::Role::kAssistant: role = "assistant"; break;
      case Message::Role::kSystem: role = "system"; break;
    }
    messages.push_back({{"role", role}, {"content", msg.content}});
  }
  req["messages"] = messages;
  return req.dump();
}

// ============================================================================
// Public API
// ============================================================================
OllamaBackend::OllamaBackend(Config config,
                             std::unique_ptr<pu::http::HttpClient> http)
    : Backend(std::move(config)), config_(config), http_(std::move(http)) {}

void OllamaBackend::Chat(const std::vector<Message>& history, ChatCallback cb) {
  std::string body = BuildRequest(history);
  std::string url = "http://localhost:11434/api/chat";  // TODO: make configurable

  std::string line_buffer;
  std::string current_content;

  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    line_buffer.append(ptr, total);
    size_t pos = 0;
    while ((pos = line_buffer.find('\n')) != std::string::npos) {
      std::string line = line_buffer.substr(0, pos);
      line_buffer.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();

      auto token_opt = ParseSseLine(line);
      if (!token_opt) continue;

      const auto& token = *token_opt;
      if (token.done) {
        cb(TokenType::kContent, "", true);
        return total;
      } else if (!token.content.empty()) {
        current_content += token.content;
        cb(TokenType::kContent, token.content, false);
      }
    }
    return total;
  };

  http_->PostStream(url, body, {"Content-Type: application/json"}, write_cb);
}

}  // namespace pu::backends::ollama
