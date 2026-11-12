// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// What it takes to answer a turn: one method, and the two structures it reads and returns.
//
// The provider itself is the narrow part of this layer. Everything it is asked and everything it
// answers with lives in `llm_types.hpp`, which the session, the tools and the runtime all include
// without needing to know that a provider exists; only the code that actually reaches a network
// needs this file.
//

#include "pu/llm/llm_types.hpp"

namespace pu {

class LLMProvider {
 public:
  virtual ~LLMProvider() = default;

  virtual ChatResult Chat(const ChatRequest& request) = 0;
};

}  // namespace pu
