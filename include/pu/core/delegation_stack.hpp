// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/core/context.hpp"
#include "pu/core/delegation.hpp"

#include <memory>
#include <vector>

namespace pu::core {

class DelegationStack {
 public:
  struct Frame {
    Delegation delegation;
    std::shared_ptr<Context> context;
  };

  DelegationStack() = default;
  explicit DelegationStack(std::shared_ptr<Context> root_context);

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

  void Clear();

 private:
  std::shared_ptr<Context> root_context_;
  std::vector<Frame> frames_;
};

}  // namespace pu::core
