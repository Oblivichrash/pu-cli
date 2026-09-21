// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The request view: the message list a provider receives for one turn.
//
// It lives in the session layer rather than in context/ because it renders the
// legacy ChatMessage that LLMProvider still requires, and that conversion is the
// compatibility seam. A pure view over the graph needs no provider, so it is
// testable on its own.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "pu/context/graph.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu::session {

// Supplied by the caller rather than stored, so that what reaches the model
// depends only on the conversation plus these named inputs.
struct RequestInputs {
  std::string system_prompt;  // the agent's configured prompt
  std::string environment;    // generated context: OS, policy, guidelines
};

// How much of the path a request carries: the first `head` and last `tail`
// nodes. Nothing is removed from storage, so the same conversation renders
// differently for different callers and a later request can carry more of it.
struct KeepRecent {
  std::size_t head = 0;
  std::size_t tail = 0;
};

// Renders one stored node as the message a provider understands. Also used by
// the transcript for its history, so both views cannot drift apart.
ChatMessage RenderMessage(const context::MessageNode& node, int position);

// The messages for the turn ending at `leaf`: the system inputs first, then the
// selected part of the stored path in order. Nodes already in storage are passed
// through untouched, including a system node a caller appended.
//
// With no selection the whole path is sent. With one, the omitted middle is
// represented by a marker message so the model can tell that a range is missing,
// and a tool call is never separated from the receipt that answers it.
std::vector<ChatMessage> BuildRequestPath(const context::MessageGraph& graph,
                                          const context::MessageId& leaf,
                                          const RequestInputs& inputs,
                                          std::optional<KeepRecent> selection = std::nullopt);

}  // namespace pu::session
