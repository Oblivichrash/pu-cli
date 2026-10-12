// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

#include "pu/core/base.hpp"

namespace pu::llm {

// The gateway accepts a generated id where the real client sends an account user id.
inline std::string CodeBuddyUserId() { return uuid::Generate(); }

// The server groups a conversation's turns by this header, so it must outlive a
// single request; the per-request ids below are what vary between calls.
inline std::string CodeBuddyConversationId() { return uuid::Generate(); }

// Headers the installed client sends; which the server requires is not isolated.
// `conversation_id` is supplied by the caller so that every turn of a session is
// one conversation server-side.
inline std::vector<std::string> CodeBuddyHeaders(const std::string& conversation_id) {
  constexpr const char* kClientVersion = "1.0.7";
  const std::string version = kClientVersion;
  // The client sends a bare 32-char hex id where the value is not a uuid.
  const auto hex_id = [] {
    std::string hex;
    hex.reserve(32);
    for (const char c : uuid::Generate()) {
      if (c != '-') hex += c;
    }
    return hex;
  };
  return {
      "X-Requested-With: XMLHttpRequest",
      "X-Domain: www.codebuddy.ai",
      "X-Product: SaaS",
      "X-Agent-Intent: craft",
      "X-IDE-Type: CLI",
      "X-IDE-Name: CLI",
      "X-IDE-Version: " + version,
      "User-Agent: CLI/" + version + " CodeBuddy/" + version,
      "X-Conversation-ID: " + conversation_id,
      "X-Conversation-Request-ID: " + hex_id(),
      "X-Conversation-Message-ID: " + hex_id(),
      "X-Request-ID: " + hex_id(),
      "X-User-Id: " + CodeBuddyUserId(),
      "x-stainless-lang: js",
      "x-stainless-package-version: 5.10.1",
      "x-stainless-runtime: node",
  };
}

}  // namespace pu::llm
