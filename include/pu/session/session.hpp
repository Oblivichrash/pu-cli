// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <string>

#include <boost/json.hpp>

#include "pu/session/workspace.hpp"
#include "pu/llm/llm_provider.hpp"
#include "pu/agent_config.hpp"
#include "pu/core/json.hpp"

namespace pu {

// The session names the agent it is talking to, and carries a backend only when
// the user overrode one for this session. Everything else about the backend is
// read from agents.json, so editing the configuration takes effect on restart.
struct RuntimeSpec {
  std::string agent_name;
  std::optional<config::BackendConfig> backend_override;

  boost::json::value Serialize() const {
    boost::json::value jv = {{"agent_name", agent_name}};
    if (backend_override) {
      jv.as_object()["backend_override"] = boost::json::value_from(*backend_override);
    }
    return jv;
  }

  // A section that is not an object cannot name an agent, so it is refused
  // rather than read into a spec that would start the wrong model.
  static std::optional<RuntimeSpec> Deserialize(const boost::json::value& jv) {
    if (!jv.is_object()) return std::nullopt;

    RuntimeSpec spec;
    if (json::HasKey(jv, "backend_override")) {
      spec.backend_override =
          boost::json::value_to<config::BackendConfig>(jv.at("backend_override"));
    }
    spec.agent_name = json::ValueOrDefault<std::string>(jv, "agent_name", "");
    return spec;
  }
};

class Session {
public:
  Session();
  Session(std::shared_ptr<Workspace> workspace, const RuntimeSpec& spec);
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) = default;
  Session& operator=(Session&&) = default;

  Workspace& GetWorkspace() { return *workspace_; }
  const Workspace& GetWorkspace() const { return *workspace_; }
  RuntimeSpec& GetRuntimeSpec() { return runtime_spec_; }
  const RuntimeSpec& GetRuntimeSpec() const { return runtime_spec_; }

  void SetBackendOverride(const config::BackendConfig& new_config);
  void SetAgent(const std::string& agent_name);

  bool HasPendingToolCalls() const { return workspace_->HasPendingToolCalls(); }

  std::unique_ptr<LLMProvider> CreateProvider(const config::BackendConfig& backend) const;

  boost::json::value Serialize() const;
  static std::unique_ptr<Session> Deserialize(const boost::json::value& j);

private:
  std::shared_ptr<Workspace> workspace_;
  RuntimeSpec runtime_spec_;
};

} // namespace pu
