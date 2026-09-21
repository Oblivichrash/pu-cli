// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The request view: the message list a provider receives for one turn.
//
// It lives in the session layer rather than in context/ because it renders the
// legacy ChatMessage that LLMProvider still requires, and that conversion is the
// compatibility seam. A pure view over the graph needs no provider, so it is
// testable on its own.

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

// Renders one stored node as the message a provider understands. Also used by
// the transcript for its history, so both views cannot drift apart.
ChatMessage RenderMessage(const context::MessageNode& node, int position);

// The messages for the turn ending at `leaf`: the system inputs first, then the
// stored path in order. Nodes already in storage are passed through untouched,
// including a system node a compaction policy appended.
std::vector<ChatMessage> BuildRequestPath(const context::MessageGraph& graph,
                                          const context::MessageId& leaf,
                                          const RequestInputs& inputs);

}  // namespace pu::session
