// SPDX-License-Identifier: GPL-3.0-only
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
  std::string reasoning;
  std::vector<backend::ToolCall> tool_calls;
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

    bool has_content = false;
    if (j.contains("choices") && !j["choices"].empty()) {
      auto& choice = j["choices"][0];
      if (choice.contains("delta")) {
        auto& delta = choice["delta"];
        if (delta.contains("content") && !delta["content"].is_null()) {
          token.content = delta["content"].get<std::string>();
          has_content = true;
        }
        if (delta.contains("reasoning_content") && !delta["reasoning_content"].is_null()) {
          token.reasoning = delta["reasoning_content"].get<std::string>();
          has_content = true;
        } else if (delta.contains("reasoning") && !delta["reasoning"].is_null()) {
          token.reasoning = delta["reasoning"].get<std::string>();
          has_content = true;
        }
        if (delta.contains("tool_calls")) {
          for (const auto& tc : delta["tool_calls"]) {
            backend::ToolCall call;
            if (tc.contains("id")) call.id = tc["id"].get<std::string>();
            if (tc.contains("function")) {
              call.name = tc["function"].value("name", "");
              if (tc["function"].contains("arguments")) {
                const auto& args = tc["function"]["arguments"];
                if (args.is_string()) {
                  call.arguments = args.get<std::string>();
                } else if (args.is_object() || args.is_array()) {
                  call.arguments = args.dump();
                }
              }
            }
            token.tool_calls.push_back(call);
          }
          has_content = true;
        }
      }
    } else if (!j.contains("done")) {
      return std::nullopt;
    }

    if (!has_content && !token.done) return std::nullopt;

    return token;
  } catch (const std::exception&) {
    throw std::runtime_error("JSON parse error in SSE line: " + std::string(line));
  }
}

}  // namespace pu::backends::openai::internal
