// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

// The facts about the CodeBuddy cloud gateway that the rest of the code would
// otherwise have to repeat. Everything here was observed against a real account;
// `docs/design/codebuddy-cloud-api.md` records the observations.

namespace pu::llm {

// Where the gateway lives. The provider appends "/chat/completions", so this is
// the path up to and including the version segment.
inline constexpr const char* kCodeBuddyHost = "https://copilot.tencent.com/v2";

// The headers a request is sent with: the values the installed client sends, quoted,
// and the only combination known to be accepted — which of them the server actually
// requires has not been isolated. The correlation ids are generated per call, because
// the gateway groups a conversation by them while the conversation itself travels in
// `messages`.
std::vector<std::string> CodeBuddyHeaders();

}  // namespace pu::llm
