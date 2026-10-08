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

// The headers a request is sent with.
//
// These are the values the installed CodeBuddy client sends, quoted, and the only
// combination known to be accepted: which of them the server actually requires has
// not been isolated, so the whole set goes out. Two of them are the client naming
// itself; whether the gateway minds a different name is worth trying once someone
// can spend the requests to find out.
//
// The correlation ids are generated per call. The endpoint echoes a conversation
// id back as the chunk id, and groups a conversation by it, while the conversation
// itself travels in `messages`, so fresh ids still reach the model with its
// history.
std::vector<std::string> CodeBuddyHeaders();

}  // namespace pu::llm
