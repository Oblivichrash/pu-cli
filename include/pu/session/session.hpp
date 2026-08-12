// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <chrono>
#include <nlohmann/json.hpp>
#include "pu/session/workspace.hpp"
#include "pu/llm/llm_provider.hpp"
#include "pu/agent_config.hpp"

namespace pu {

// Serialized via NLOHMANN_DEFINE_TYPE_INTRUSIVE using the custom to_json/from_json in agent_config.hpp.
struct RuntimeSpec {
  config::BackendConfig backend;
  std::string agent_name;

  nlohmann::json Serialize() const { return nlohmann::json(*this); }
  static RuntimeSpec Deserialize(const nlohmann::json& j) { return j.get<RuntimeSpec>(); }

  NLOHMANN_DEFINE_TYPE_INTRUSIVE(RuntimeSpec, backend, agent_name)
};

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

  std::string GetId() const { return id_; }

  Workspace& GetWorkspace() { return *workspace_; }
  const Workspace& GetWorkspace() const { return *workspace_; }
  RuntimeSpec& GetRuntimeSpec() { return runtime_spec_; }
  const RuntimeSpec& GetRuntimeSpec() const { return runtime_spec_; }

  void SwitchBackend(const config::BackendConfig& new_config);
  void SwitchAgent(const std::string& agent_name);
  void Touch() { last_access_at_ = std::chrono::system_clock::now(); }

  bool HasPendingToolCalls() const { return workspace_->HasPendingToolCalls(); }

  std::unique_ptr<LLMProvider> CreateProvider() const;

  nlohmann::json Serialize() const;
  static std::unique_ptr<Session> Deserialize(const nlohmann::json& j);

private:
  std::string id_;
  std::string owner_id_;
  std::chrono::system_clock::time_point created_at_;
  std::chrono::system_clock::time_point last_access_at_;
  std::shared_ptr<Workspace> workspace_;
  RuntimeSpec runtime_spec_;
};

} // namespace pu
