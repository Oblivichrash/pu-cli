// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// Internal SSE parsing utilities for Ollama backend.

#pragma once

#include "pu/backend.hpp"
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pu::backends::ollama::internal {

struct SseToken {
  std::string content;
  std::string reasoning;                  // thinking/reasoning content
  std::vector<backend::ToolCall> tool_calls;  // tool calls if any
  bool done = false;
};

inline std::optional<SseToken> ParseSseLine(std::string_view line) {
  if (line.empty()) return std::nullopt;
  try {
    using json = nlohmann::json;
    json j = json::parse(line);
    SseToken token;

    if (j.contains("done") && j["done"].get<bool>()) {
      token.done = true;
    }
    if (j.contains("message")) {
      if (j["message"].contains("content")) {
        token.content = j["message"]["content"];
      }
      if (j["message"].contains("thinking")) {
        token.reasoning = j["message"]["thinking"];
      }
      if (j["message"].contains("tool_calls")) {
        for (const auto& tc : j["message"]["tool_calls"]) {
          backend::ToolCall call;
          if (tc.contains("id")) call.id = tc["id"].get<std::string>();
          if (tc.contains("function")) {
            call.name = tc["function"].value("name", "");
            if (tc["function"].contains("arguments")) {
              call.arguments = tc["function"]["arguments"].dump(); // keep JSON string
            }
          }
          token.tool_calls.push_back(call);
        }
      }
    }
    if (!token.done && token.content.empty() && token.reasoning.empty() && token.tool_calls.empty()) {
      return std::nullopt;
    }
    return token;
  } catch (const std::exception&) {
    throw std::runtime_error("JSON parse error in SSE line: " + std::string(line));
  }
}

}  // namespace pu::backends::ollama::internal
