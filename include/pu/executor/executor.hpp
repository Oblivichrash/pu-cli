// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pu/llm/llm_provider.hpp"
#include "pu/tools/toolbox.hpp"
#include "pu/session/workspace.hpp"
#include "pu/session/call_stack.hpp"
#include "pu/conversation.hpp"

namespace pu {

class Executor {
 public:
  Executor(Toolbox* toolbox);

  std::string Execute(const std::string& input,
                      Workspace& workspace,
                      LLMProvider* provider);

 private:
  struct ToolLoopResult {
    std::string final_response;
  };

  ToolLoopResult RunToolLoop(Workspace& workspace, LLMProvider* provider);

  Toolbox* toolbox_;
};

}  // namespace pu
