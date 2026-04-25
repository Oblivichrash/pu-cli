// SPDX-License-Identifier: GPL-3.0-only
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
  std::string reasoning;
  std::vector<backend::ToolCall> tool_calls;
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
      return token;
    }

    bool has_content = false;
    if (j.contains("message")) {
      if (j["message"].contains("content")) {
        token.content = j["message"]["content"];
        has_content = true;
      }
      if (j["message"].contains("thinking")) {
        token.reasoning = j["message"]["thinking"];
        has_content = true;
      }
      if (j["message"].contains("tool_calls")) {
        for (const auto& tc : j["message"]["tool_calls"]) {
          backend::ToolCall call;
          if (tc.contains("id")) call.id = tc["id"].get<std::string>();
          if (tc.contains("function")) {
            call.name = tc["function"].value("name", "");
            if (tc["function"].contains("arguments")) {
              call.arguments = tc["function"]["arguments"].get<std::string>();
            }
          }
          token.tool_calls.push_back(call);
        }
        has_content = true;
      }
    }

    if (!has_content) return std::nullopt;

    return token;
  } catch (const std::exception&) {
    throw std::runtime_error("JSON parse error in SSE line: " + std::string(line));
  }
}

}  // namespace pu::backends::ollama::internal
