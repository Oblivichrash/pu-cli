// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>

#include "pu/agent_core.hpp"
#include "pu/session/workspace.hpp"
#include "pu/session/assignment.hpp"

namespace pu {

class SummaryGenerator {
 public:
  explicit SummaryGenerator(agent::AgentManager& manager);

  HandoffReceipt Generate(const std::shared_ptr<Workspace>& child_ctx,
                         const Assignment& delegation);

 private:
  agent::AgentManager& manager_;
};

}  // namespace pu
