// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// Internal SSE parsing utilities for Ollama backend.

#ifndef PU_BACKENDS_OLLAMA_SSE_PARSER_HPP
#define PU_BACKENDS_OLLAMA_SSE_PARSER_HPP

#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pu::backends::ollama::internal {

struct SseToken {
  std::string content;
  bool done = false;
};

inline std::optional<SseToken> ParseSseLine(std::string_view line) {
  if (line.empty()) return std::nullopt;
  try {
    nlohmann::json j = nlohmann::json::parse(line);
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

}  // namespace pu::backends::ollama::internal

#endif  // PU_BACKENDS_OLLAMA_SSE_PARSER_HPP
