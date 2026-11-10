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
