// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>

#include "pu/agent.hpp"
#include "pu/core/base.hpp"
#include "pu/executor.hpp"
#include "pu/mcp/client.hpp"
#include "pu/command_router.hpp"
#include "pu/session/session.hpp"
#include "pu/tools/toolbox.hpp"

namespace pu {

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
      std::function<void(const std::string&)> content_callback = nullptr,
      ToolCallbacks tool_callbacks = {},
      std::function<void(const std::string&)> reasoning_callback = nullptr);

  void SetDefaultAgent(const std::string& agent_name);
  void SwitchAgent(const config::AgentEntry& new_agent);

  bool SwitchWorkspace(const std::filesystem::path& new_root);
  std::vector<std::pair<std::string, std::string>> ListWorkspaces() const;
  std::filesystem::path GetWorkspaceRoot() const { return workspace_root_; }
  std::string GetWorkspaceName() const { return workspace_root_.filename().string(); }

  AgentManager& GetAgentManager() { return *agent_manager_; }
  std::shared_ptr<Session> GetOrCreateDefaultSession();

  // The two conversation changes the Web API exposes. Both write the session,
  // because the Runtime is the only place that knows a session is on disk at all:
  // a caller that changed the workspace itself would be a caller that can forget
  // to persist it.
  bool RewindBefore(size_t turn);
  void ClearConversation();

  // The backend the active session talks to: the session's override when it has
  // one, otherwise the agent's entry in agents.json, with the thinking level the
  // session asked for applied on top.
  config::BackendConfig CurrentBackend() const;

  // What this session asks for, which is what a caller displays: the session's
  // override when it has one, the agent's configuration otherwise.
  ThinkingLevel CurrentThinkingLevel() const;
  // Whether the active backend carries a level at all, so a caller offers the
  // setting only where it lands.
  bool SupportsThinkingLevel() const;
  // Absent means the session follows the agent's configuration rather than a level
  // of its own.
  std::optional<ThinkingLevel> GetThinkingOverride() const;
  // Sets or clears the session's level and persists it, the way the other session
  // changes are persisted: a caller that changed the session is not one that can
  // be relied on to remember it. Answers false, changing nothing, when the active
  // backend does not carry a level, so both front ends refuse the same way.
  bool SetThinkingLevel(std::optional<ThinkingLevel> level);

 private:
  // The backend as configured, before anything this session asked for is applied.
  config::BackendConfig ConfiguredBackend() const;
  void RebuildToolbox(const config::AgentEntry& agent);
  void SaveCurrentSession();
  void ShutdownMCP();
  bool StartMCP(const pu::mcp::McpServerConfig& config);
  void RegisterBuiltinTools(const config::AgentEntry& agent);

  bool is_initialized_ = false;
  bool is_running_ = false;
  std::unique_ptr<AgentManager> agent_manager_;
  std::unique_ptr<CommandRouter> command_router_;
  std::unique_ptr<Toolbox> toolbox_;
  std::unique_ptr<Executor> executor_;
  std::filesystem::path workspace_root_;
  std::shared_ptr<Session> current_session_;

  std::string default_agent_override_;

  std::vector<std::unique_ptr<mcp::McpClient>> mcp_clients_;
};

}  // namespace pu
