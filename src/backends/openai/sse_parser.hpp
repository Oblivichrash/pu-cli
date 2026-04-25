// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// Internal SSE parsing utilities for OpenAI-compatible backends.

#pragma once

#include "pu/backend.hpp"
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pu::backends::openai::internal {

struct SseToken {
  std::string content;
  std::string reasoning;                  // reasoning_content from OpenAI o1 / compatible services
  std::vector<backend::ToolCall> tool_calls;  // tool calls if any
  bool done = false;
};

inline std::optional<SseToken> ParseSseLine(std::string_view line) {
  constexpr std::string_view kDataPrefix = "data: ";

  size_t start = line.find_first_not_of(" \t");
  if (start == std::string_view::npos) return std::nullopt;
  std::string_view trimmed = line.substr(start);

  if (trimmed.substr(0, kDataPrefix.size()) != kDataPrefix) {
    return std::nullopt;
  }
  std::string_view data = trimmed.substr(kDataPrefix.size());
  if (data == "[DONE]") {
    SseToken token;
    token.done = true;
    return token;
  }

  try {
    using json = nlohmann::json;
    json j = json::parse(data);
    SseToken token;
    token.done = false;

    bool has_meaningful_content = false;
    if (j.contains("choices") && !j["choices"].empty()) {
      auto& choice = j["choices"][0];
      if (choice.contains("delta")) {
        if (choice["delta"].contains("content")) {
          token.content = choice["delta"]["content"];
          has_meaningful_content = true;
        }
        if (choice["delta"].contains("reasoning")) {
          token.reasoning = choice["delta"]["reasoning"];
          has_meaningful_content = true;
        }
        if (choice["delta"].contains("tool_calls")) {
          for (const auto& tc : choice["delta"]["tool_calls"]) {
            backend::ToolCall call;
            if (tc.contains("id")) call.id = tc["id"].get<std::string>();
            if (tc.contains("function")) {
              call.name = tc["function"].value("name", "");
              if (tc["function"].contains("arguments")) {
                call.arguments = tc["function"]["arguments"].dump();
              }
            }
            token.tool_calls.push_back(call);
          }
          has_meaningful_content = true;
        }
      }
      // Even if no delta content, the presence of choices constitutes a valid token
      if (!has_meaningful_content) {
        // Token is empty but valid (e.g., heartbeat)
        return token;
      }
    } else {
      // No choices: this line has no useful data; ignore unless done
      if (!j.contains("done")) {
        return std::nullopt;
      }
    }

    return token;
  } catch (const std::exception&) {
    throw std::runtime_error("JSON parse error in SSE line: " + std::string(line));
  }
}

}  // namespace pu::backends::openai::internal
