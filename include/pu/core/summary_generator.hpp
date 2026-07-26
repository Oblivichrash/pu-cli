// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>

#include "pu/agent_core.hpp"
#include "pu/core/context.hpp"
#include "pu/core/delegation.hpp"

namespace pu::core {

class SummaryGenerator {
 public:
  explicit SummaryGenerator(agent::AgentManager& manager);

  SummaryReport Generate(const std::shared_ptr<Context>& child_ctx,
                         const Delegation& delegation);

 private:
  agent::AgentManager& manager_;
};

}  // namespace pu::core
