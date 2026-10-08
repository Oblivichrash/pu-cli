// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/codebuddy.hpp"

#include "pu/core/base.hpp"

namespace pu::llm {

namespace {

// The build the installed client reports itself as.
constexpr const char* kClientVersion = "1.0.7";

// A bare 32-character hex id. uuid::Generate is hyphenated, and the client sends
// this shape where the value is not a uuid.
std::string HexId() {
  const std::string id = uuid::Generate();
  std::string hex;
  hex.reserve(32);
  for (const char c : id) {
    if (c != '-') hex += c;
  }
  return hex;
}

}  // namespace

std::vector<std::string> CodeBuddyHeaders() {
  const std::string version = kClientVersion;
  return {
      "X-Requested-With: XMLHttpRequest",
      "X-Domain: www.codebuddy.ai",
      "X-Product: SaaS",
      "X-Agent-Intent: craft",
      "X-IDE-Type: CLI",
      "X-IDE-Name: CLI",
      "X-IDE-Version: " + version,
      "User-Agent: CLI/" + version + " CodeBuddy/" + version,
      "X-Conversation-ID: " + uuid::Generate(),
      "X-Conversation-Request-ID: " + HexId(),
      "X-Conversation-Message-ID: " + HexId(),
      "X-Request-ID: " + HexId(),
      // The client sends its account's user id here. pu-cli has no separate
      // identity of its own to offer, and the gateway accepted a generated one,
      // so a generated one is what goes out.
      "X-User-Id: " + uuid::Generate(),
      "x-stainless-lang: js",
      "x-stainless-package-version: 5.10.1",
      "x-stainless-runtime: node",
  };
}

}  // namespace pu::llm
