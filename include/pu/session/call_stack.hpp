// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <vector>
#include <memory>
#include <optional>
#include <nlohmann/json.hpp>
#include "pu/session/assignment.hpp"
#include "pu/session/workspace.hpp"

namespace pu {

class ForkMergeService;
class AgentManager;

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
  std::shared_ptr<Workspace> CurrentContext() const;
  size_t Depth() const;
  bool IsEmpty() const;
  void Clear();

  // Methods for integration with ForkMergeService
  std::shared_ptr<Workspace> GetRootContext() const { return root_context_; }
  void SetRootContext(std::shared_ptr<Workspace> ctx) { root_context_ = ctx; }
  std::shared_ptr<ForkMergeService> GetForkMergeService() const { return fork_service_; }
  void SetForkMergeService(std::shared_ptr<ForkMergeService> svc) { fork_service_ = svc; }
  const std::vector<Frame>& GetFrames() const { return frames_; }

  // Static factory to create a CallStack with ForkMergeService
  static std::shared_ptr<CallStack> Create(
    std::shared_ptr<Workspace> root_context,
    AgentManager& manager);

  nlohmann::json Serialize() const;
  static CallStack Deserialize(const nlohmann::json& j);

private:
  std::shared_ptr<Workspace> root_context_;
  std::vector<Frame> frames_;
  std::shared_ptr<ForkMergeService> fork_service_;
};

} // namespace pu
