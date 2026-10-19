// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cassert>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pu/config/agents.hpp"
#include "pu/config/peer.hpp"
#include "pu/core/base.hpp"
#include "pu/core/http_client.hpp"
#include "pu/runtime/executor.hpp"
#include "pu/mcp/client.hpp"
#include "pu/runtime/command_router.hpp"
#include "pu/session/session.hpp"
#include "pu/tools/toolbox.hpp"

namespace pu {


class AgentManager {
 public:
  AgentManager() = default;

  void LoadAgentConfigs(const std::vector<config::AgentEntry>& configs) {
    agent_configs_ = configs;
  }

  const config::AgentEntry* AgentConfig(const std::string& name) const {
    for (const auto& entry : agent_configs_) {
      if (entry.name == name) return &entry;
    }
    for (const auto& entry : transient_agents_) {
      if (entry.name == name) return &entry;
    }
    return nullptr;
  }

  const config::AgentEntry& Adopt(const config::AgentEntry& entry) {
    for (auto& existing : transient_agents_) {
      if (existing.name == entry.name) {
        existing = entry;
        return existing;
      }
    }
    transient_agents_.push_back(entry);
    return transient_agents_.back();
  }

  std::vector<std::string> AgentNames() const {
    std::vector<std::string> names;
    names.reserve(agent_configs_.size() + transient_agents_.size());
    for (const auto& entry : agent_configs_) names.push_back(entry.name);
    for (const auto& entry : transient_agents_) names.push_back(entry.name);
    return names;
  }

  void SetActiveAgent(const std::string& name) { active_agent_ = name; }
  std::string ActiveAgentName() const { return active_agent_; }

 private:
  std::string active_agent_;
  std::vector<config::AgentEntry> agent_configs_;
  std::vector<config::AgentEntry> transient_agents_;
};


class Runtime {
 public:
  Runtime() = default;
  ~Runtime() = default;

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  void Initialize(const std::string& config_path = "");
  void Shutdown();

  ExecutionResult ProcessInput(
      const std::string& input, bool& is_command, CancelToken cancel_token = nullptr,
      TokenCallback content_callback = nullptr,
      ToolCallbacks tool_callbacks = {},
      TokenCallback reasoning_callback = nullptr,
      CancelToken command_stop_token = nullptr,
      // The peer this turn arrived from, empty for a turn the user started. A turn from a
      // peer must not reach another one.
      const std::string& peer_origin = {});

  // What `.pu/peers.json` named, or nothing when this server is not part of a topology.
  const std::optional<config::PeerConfig>& Peers() const { return peer_config_; }

  void SetDefaultAgent(const std::string& agent_name);
  void SwitchAgent(const config::AgentEntry& new_agent);
  void SwitchBackend(const config::Backend& backend);

  std::vector<std::pair<std::string, std::string>> ListWorkspaces() const;
  std::filesystem::path WorkspaceRoot() const { return workspace_root_; }
  // The file Initialize() loaded.
  const std::string& ConfigPath() const { return config_path_; }

  Toolbox& Tools() {
    assert(is_initialized_ && "Initialize() must run before any toolbox query");
    return toolbox_;
  }

  // Words typed while a turn runs. The next tool boundary carries them to the
  // model; a command killed through the token names them in its result.
  void SetUserNote(std::string note) {
    assert(is_initialized_ && "Initialize() must run before any note");
    executor_->SetUserNote(std::move(note));
  }

  // The approval gate. Ask fires on the turn thread when a command matches the
  // policy's approval patterns; Resolve answers it from the serve thread.
  void EnableApprovalGate() {
    assert(is_initialized_ && "Initialize() must run before any gateway");
    executor_->EnableApprovalGate();
  }
  void SetOnApprovalAsk(std::function<void(const std::string&)> cb) {
    executor_->SetOnApprovalAsk(std::move(cb));
  }
  void ResolveApproval(bool allow, std::string note) {
    executor_->ResolveApproval(allow, std::move(note));
  }

  AgentManager& Agents() {
    assert(agent_manager_ && "Initialize() must run before any agent query");
    return *agent_manager_;
  }
  // Reading who is configured does not change anything, so it is answered from a const
  // Runtime as well: the summary a surface prints is a read.
  const AgentManager& Agents() const {
    assert(agent_manager_ && "Initialize() must run before any agent query");
    return *agent_manager_;
  }
  pu::http::HttpClient& Http() {
    assert(is_initialized_ && "Initialize() must run before any HTTP call");
    return *http_client_;
  }
  // For a collaborator that outlives the call and needs the client shared, which a
  // provider built per model does.
  std::shared_ptr<pu::http::HttpClient> SharedHttp() {
    assert(is_initialized_ && "Initialize() must run before any HTTP call");
    return http_client_;
  }
  std::shared_ptr<Session> DefaultSession();

  std::string HelpText();

  const config::AgentEntry& ActiveAgent() const;

  bool RewindBefore(size_t turn);
  void ClearConversation();

  config::Backend CurrentBackend() const;

  ThinkingLevel CurrentThinkingLevel() const;
  std::optional<ThinkingLevel> ThinkingOverride() const;
  void SetThinkingLevel(std::optional<ThinkingLevel> level);

  // What this session overrides, and what is in force once the overrides are applied to the
  // agent's own configuration. `Settings()` renders both the value and where it came from.
  const session::Overrides& Overrides() const;
  std::vector<session::SettingView> Settings() const;
  // One setting from a request: `null` clears it, anything else is checked against its row.
  void SetSetting(const std::string& name, const boost::json::value& raw);
  void ClearSettings();

 private:
  config::Backend ConfiguredBackend() const;
  // The budget the agent's configuration names, which is what a cleared override returns to.
  int ConfiguredToolBudget() const;
  // Pushes the budget in force into the executor, after an override changed or an agent switch.
  void ApplyToolBudget();
  void Activate(const config::AgentEntry& agent);
  void RebuildToolbox(const config::AgentEntry& agent);
  void SaveCurrentSession();
  void RegisterBuiltinTools(const config::AgentEntry& agent);
  void RegisterMcpTools();

  bool is_initialized_ = false;
  std::unique_ptr<AgentManager> agent_manager_;
  std::unique_ptr<CommandRouter> command_router_;
  // Shared so the TLS context, built from the OS certificate store, outlives a single turn.
  std::shared_ptr<pu::http::HttpClient> http_client_;
  Toolbox toolbox_;
  std::unique_ptr<Executor> executor_;
  std::filesystem::path workspace_root_;
  std::shared_ptr<Session> current_session_;

  std::string default_agent_override_;
  std::optional<config::PeerConfig> peer_config_;
  std::string config_path_;

  std::vector<std::shared_ptr<mcp::McpClient>> mcp_clients_;
};

}  // namespace pu
