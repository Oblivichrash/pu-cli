// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "pu/agent.hpp"
#include "pu/context/graph.hpp"
#include "pu/core/json.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu {

// The stored conversation.
//
// FROZEN: the public surface does not change. Storage behind it is the
// MessageGraph (include/pu/context/graph.hpp); the compatibility seam is the
// ChatMessage view this class renders from it.
class Transcript {
 public:
  void Append(const ChatMessage& msg);
  std::vector<ChatMessage> GetHistory() const;
  bool HasPendingToolCalls() const;
  size_t Size() const;

  // The stored conversation, for a caller that renders its own view of it.
  const context::MessageGraph& GetGraph() const { return graph_; }

  // Moves the position back to just before the 1-based turn, keeping every
  // stored node so the abandoned branch can still be read.
  bool RewindBefore(size_t turn);

  boost::json::value Serialize() const;
  // Returns false for a value that is not DAG storage, so the caller can tell a
  // foreign layout from an empty conversation.
  static bool Deserialize(const boost::json::value& j, Transcript& out);

 private:
  context::MessageGraph graph_;
};

// The state that outlives one request. It holds the conversation and nothing
// else: the agent and the backend are named by the RuntimeSpec a Session
// carries beside it.
class Workspace {
 public:
  Workspace() = default;

  void Append(const ChatMessage& msg);
  void Append(const std::string& role, const std::string& content);
  std::vector<ChatMessage> GetHistory() const;
  size_t HistorySize() const;
  bool HasPendingToolCalls() const;

  // The stored conversation, for a caller that renders its own view of it.
  const context::MessageGraph& GetGraph() const { return transcript_.GetGraph(); }

  bool RewindBefore(size_t turn);

  void ClearHistory();

  boost::json::value Serialize() const;
  static std::shared_ptr<Workspace> Deserialize(const boost::json::value& j);

 private:
  Transcript transcript_;
};

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

// Aggregate root: the conversation plus the agent and backend it belongs to.
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
  // Choosing an agent drops the override, so the agent's own configuration
  // becomes the source of the backend again.
  void SetAgent(const std::string& agent_name);

  bool HasPendingToolCalls() const { return workspace_->HasPendingToolCalls(); }

  std::unique_ptr<LLMProvider> CreateProvider(const config::BackendConfig& backend) const;

  boost::json::value Serialize() const;
  static std::unique_ptr<Session> Deserialize(const boost::json::value& j);

 private:
  std::shared_ptr<Workspace> workspace_;
  RuntimeSpec runtime_spec_;
};

}  // namespace pu
