// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <chrono>
#include <nlohmann/json.hpp>
#include "pu/session/workspace.hpp"
#include "pu/session/runtime_spec.hpp"
#include "pu/session/call_stack.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu {

class Session {
public:
  Session(const std::string& id, const std::string& owner_id);
  Session(const std::string& id, const std::string& owner_id,
          std::shared_ptr<Workspace> workspace,
          const RuntimeSpec& spec);
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) = default;
  Session& operator=(Session&&) = default;

  // Accessors
  std::string GetId() const { return id_; }

  Workspace& GetWorkspace() { return *workspace_; }
  const Workspace& GetWorkspace() const { return *workspace_; }
  RuntimeSpec& GetRuntimeSpec() { return runtime_spec_; }
  const RuntimeSpec& GetRuntimeSpec() const { return runtime_spec_; }

  std::shared_ptr<CallStack> GetCallStackPtr() const { return call_stack_; }
  CallStack& GetCallStack() { return *call_stack_; }
  const CallStack& GetCallStack() const { return *call_stack_; }

  // Core operations
  void SwitchBackend(const SessionBackendConfig& new_config);
  void SwitchAgent(const std::string& agent_name);
  void Touch() { last_access_at_ = std::chrono::system_clock::now(); }

  // Safety checks
  bool HasPendingToolCalls() const { return workspace_->HasPendingToolCalls(); }

  // Provider factory
  std::unique_ptr<LLMProvider> CreateProvider() const;

  // Serialization
  nlohmann::json Serialize() const;
  static std::unique_ptr<Session> Deserialize(const nlohmann::json& j);

private:
  std::string id_;
  std::string owner_id_;
  std::chrono::system_clock::time_point created_at_;
  std::chrono::system_clock::time_point last_access_at_;
  std::shared_ptr<Workspace> workspace_;
  RuntimeSpec runtime_spec_;
  std::shared_ptr<CallStack> call_stack_;
};

} // namespace pu
