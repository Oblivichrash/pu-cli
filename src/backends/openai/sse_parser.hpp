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
#include <vector>

namespace pu::backends::openai::internal {

struct ToolCallDelta {
  int index = -1;
  std::string id;
  std::string name;
  std::string arguments;
};

struct SseToken {
  std::string content;
  std::string reasoning;
  std::vector<ToolCallDelta> tool_call_deltas;
  bool done = false;
};

inline std::string SafeString(const nlohmann::json& j, const char* key) {
  if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
  return "";
}

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
    if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
      auto& choice = j["choices"][0];
      if (choice.contains("delta") && choice["delta"].is_object()) {
        auto& delta = choice["delta"];
        token.content = SafeString(delta, "content");
        if (!token.content.empty()) has_content = true;

        token.reasoning = SafeString(delta, "reasoning_content");
        if (token.reasoning.empty()) token.reasoning = SafeString(delta, "reasoning");
        if (!token.reasoning.empty()) has_content = true;

        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
          for (const auto& tc : delta["tool_calls"]) {
            if (!tc.is_object()) continue;
            ToolCallDelta tcd;
            tcd.index = tc.value("index", -1);
            tcd.id = SafeString(tc, "id");
            if (tc.contains("function") && tc["function"].is_object()) {
              tcd.name = SafeString(tc["function"], "name");
              tcd.arguments = SafeString(tc["function"], "arguments");
            }
            if (tcd.index >= 0) {
              token.tool_call_deltas.push_back(tcd);
            }
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
