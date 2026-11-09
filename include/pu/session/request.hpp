// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

#include "pu/session/graph.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu::session {

struct RequestInputs {
  std::string system_prompt;  // the agent's configured prompt
  std::string environment;    // generated context: OS, policy, guidelines

  // How much of the stored tool results the request may carry, in bytes. 0 means no bound,
  // which is what every caller that has not thought about this gets: shrinking a history is
  // a decision about a conversation, and one made by a default would be made by accident.
  //
  // This is a budget on the *scaffolding and observations* the turns re-send -- the outputs of
  // commands already run -- and never on what a person said. Prose is what the conversation
  // is; a tool result is what it cost to find out, and re-sending a page nobody asked for is
  // the difference between a long exploration and a full context.
  //
  // The bound is spent from the newest end: the most recent tool results are kept whole,
  // because they are the ones the current turn is reasoning about, and only the older ones can
  // be compressed. It goes in whole tens -- see `ToolBudget` -- and compressing never removes
  // a message, so a call is never separated from its receipt.
  std::size_t tool_bytes = 0;
};

// One stored node as the provider sees it. Callers that need the node's own fields alongside
// the rendered text read both from the node, rather than pairing this with a second list.
ChatMessage RenderMessage(const session::MessageNode& node, int position);

// The whole request: the system inputs, then the stored turns. A page whose source has been
// run again since is replaced by a receipt on the way out, which is what bounds a long
// exploration without touching the graph.
std::vector<ChatMessage> BuildRequestPath(const session::MessageGraph& graph,
                                          const RequestInputs& inputs);

}  // namespace pu::session
