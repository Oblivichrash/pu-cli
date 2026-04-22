// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// Internal SSE parsing utilities for OpenAI-compatible backends.

#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pu::backends::openai::internal {

struct SseToken {
  std::string content;
  bool done = false;
};

inline std::optional<SseToken> ParseSseLine(std::string_view line) {
  // OpenAI SSE format: "data: <json>" or "data: [DONE]".
  // Some servers may include extra whitespace after the colon.
  constexpr std::string_view kDataPrefix = "data: ";
  
  // Trim leading whitespace (should not be necessary for well-formed SSE,
  // but we handle it defensively).
  size_t start = line.find_first_not_of(" \t");
  if (start == std::string_view::npos) return std::nullopt;
  std::string_view trimmed = line.substr(start);
  
  if (trimmed.substr(0, kDataPrefix.size()) != kDataPrefix) {
    return std::nullopt;  // ignore comments or empty lines
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
    if (j.contains("choices") && !j["choices"].empty()) {
      auto& choice = j["choices"][0];
      if (choice.contains("delta") && choice["delta"].contains("content")) {
        token.content = choice["delta"]["content"];
      }
    }
    // Some APIs also put finish_reason in the last chunk; we rely on [DONE] instead
    return token;
  } catch (const std::exception&) {
    throw std::runtime_error("JSON parse error in SSE line: " + std::string(line));
  }
}

}  // namespace pu::backends::openai::internal
