// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>

#include "pu/agent/agent_manager.hpp"
#include "pu/session/workspace.hpp"
#include "pu/session/assignment.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu {

class SummaryGenerator {
 public:
  explicit SummaryGenerator(AgentManager& manager);

  HandoffReceipt Generate(const std::shared_ptr<Workspace>& child_ctx,
                         const Assignment& delegation,
                         LLMProvider* provider = nullptr);

 private:
  AgentManager& manager_;
};

}  // namespace pu
