// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/core/context.hpp"
#include "pu/core/delegation.hpp"

#include <memory>
#include <vector>

namespace pu {
namespace agent {
class AgentManager;
}
}  // namespace pu

namespace pu::core {

class ForkMergeService;

class DelegationStack : public std::enable_shared_from_this<DelegationStack> {
 public:
  struct Frame {
    Delegation delegation;
    std::shared_ptr<Context> context;
  };

  // Factory method to create a properly initialized DelegationStack
  static std::shared_ptr<DelegationStack> Create(
      std::shared_ptr<Context> root_context,
      agent::AgentManager& manager);

  // Private constructor - use Create() instead
  explicit DelegationStack(std::shared_ptr<Context> root_context)
      : root_context_(std::move(root_context)) {}

  void Push(const Delegation& delegation, std::shared_ptr<Context> context);
  void Push(const Delegation& delegation);

  SummaryReport Pop();

  Frame& Current();
  const Frame& Current() const;

  Delegation& CurrentDelegation();
  const Delegation& CurrentDelegation() const;

  std::shared_ptr<Context> CurrentContext();

  bool IsEmpty() const { return frames_.empty(); }
  size_t Depth() const { return frames_.size(); }
  std::shared_ptr<Context> GetRootContext() const { return root_context_; }
  const std::vector<Frame>& GetFrames() const { return frames_; }

  // Get the ForkMergeService owned by this stack
  std::shared_ptr<ForkMergeService> GetForkMergeService() const {
    return fork_service_;
  }

  void Clear();

 private:
  void Initialize(agent::AgentManager& manager);

  std::shared_ptr<Context> root_context_;
  std::vector<Frame> frames_;
  std::shared_ptr<ForkMergeService> fork_service_;
};

}  // namespace pu::core