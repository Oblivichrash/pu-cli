// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>

#include <boost/json.hpp>

#include "pu/session/workspace.hpp"
#include "pu/llm/llm_provider.hpp"
#include "pu/agent_config.hpp"
#include "pu/json.hpp"

namespace pu {

// Serialized with the custom tag_invoke conversions declared in agent_config.hpp.
struct RuntimeSpec {
  config::BackendConfig backend;
  std::string agent_name;

  boost::json::value Serialize() const {
    boost::json::value jv = {{"agent_name", agent_name}};
    jv.as_object()["backend"] = boost::json::value_from(backend);
    return jv;
  }

  static RuntimeSpec Deserialize(const boost::json::value& jv) {
    RuntimeSpec spec;
    if (json::HasKey(jv, "backend")) {
      spec.backend = boost::json::value_to<config::BackendConfig>(jv.at("backend"));
    }
    spec.agent_name = json::ValueOrDefault<std::string>(jv, "agent_name", "");
    return spec;
  }
};

class Session {
public:
  Session();
  explicit Session(std::shared_ptr<Workspace> workspace);
  Session(std::shared_ptr<Workspace> workspace, const RuntimeSpec& spec);
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) = default;
  Session& operator=(Session&&) = default;

  Workspace& GetWorkspace() { return *workspace_; }
  const Workspace& GetWorkspace() const { return *workspace_; }
  RuntimeSpec& GetRuntimeSpec() { return runtime_spec_; }
  const RuntimeSpec& GetRuntimeSpec() const { return runtime_spec_; }

  void SwitchBackend(const config::BackendConfig& new_config);
  void SwitchAgent(const std::string& agent_name);

  bool HasPendingToolCalls() const { return workspace_->HasPendingToolCalls(); }

  std::unique_ptr<LLMProvider> CreateProvider() const;

  boost::json::value Serialize() const;
  static std::unique_ptr<Session> Deserialize(const boost::json::value& j);

private:
  std::shared_ptr<Workspace> workspace_;
  RuntimeSpec runtime_spec_;
};

} // namespace pu
