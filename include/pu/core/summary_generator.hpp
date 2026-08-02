// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "pu/agent/agent_manager.hpp"
#include "pu/session/workspace.hpp"
#include "pu/session/artifact.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu {

class SummaryGenerator {
 public:
  struct SummaryResult {
    enum Status { kCompleted, kFailed, kCancelled, kTimeout };

    Status status = kCompleted;
    std::string summary;
    std::vector<Artifact> key_discoveries;
    std::vector<std::string> unresolved;
    std::chrono::milliseconds duration;
  };

  explicit SummaryGenerator(AgentManager& manager);

  SummaryResult Generate(const std::shared_ptr<Workspace>& child_workspace,
                         LLMProvider* provider = nullptr);

 private:
  AgentManager& manager_;
};

}  // namespace pu
