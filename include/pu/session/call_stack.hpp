// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <vector>
#include <memory>
#include <optional>
#include <nlohmann/json.hpp>
#include "pu/session/assignment.hpp"
#include "pu/session/workspace.hpp"

namespace pu {

class CallStack {
public:
  struct Frame {
    Assignment assignment;
    std::shared_ptr<Workspace> context;
  };

  void Push(const Assignment& assignment, std::shared_ptr<Workspace> context);
  void Push(const Assignment& assignment);
  std::optional<HandoffReceipt> Pop();
  Frame& Current();
  const Frame& Current() const;
  std::shared_ptr<Workspace> CurrentWorkspace() const;
  size_t Depth() const;
  bool IsEmpty() const;
  void Clear();

  // Workspace accessors — kept for callers that need the root workspace.
  std::shared_ptr<Workspace> GetRootWorkspace() const { return root_context_; }
  void SetRootWorkspace(std::shared_ptr<Workspace> ctx) { root_context_ = ctx; }
  const std::vector<Frame>& GetFrames() const { return frames_; }

  // Factory: creates an empty stack; callers install the root workspace.
  static std::shared_ptr<CallStack> Create(
    std::shared_ptr<Workspace> root_context);

  nlohmann::json Serialize() const;
  static CallStack Deserialize(const nlohmann::json& j);

private:
  std::shared_ptr<Workspace> root_context_;
  std::vector<Frame> frames_;
};

} // namespace pu