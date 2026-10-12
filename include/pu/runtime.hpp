// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cassert>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>

#include "pu/agent_manager.hpp"
#include "pu/config/agents.hpp"
#include "pu/core/base.hpp"
#include "pu/core/http_client.hpp"
#include "pu/executor.hpp"
#include "pu/mcp/mcp_session.hpp"
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
      TokenCallback content_callback = nullptr,
      ToolCallbacks tool_callbacks = {},
      TokenCallback reasoning_callback = nullptr);

  void SetDefaultAgent(const std::string& agent_name);
  void SwitchAgent(const config::AgentEntry& new_agent);
  void SwitchBackend(const config::BackendConfig& backend);

  std::vector<std::pair<std::string, std::string>> ListWorkspaces() const;
  std::filesystem::path GetWorkspaceRoot() const { return workspace_root_; }

  Toolbox& GetToolbox() {
    assert(is_initialized_ && "Initialize() must run before any toolbox query");
    return toolbox_;
  }

  AgentManager& GetAgentManager() {
    assert(agent_manager_ && "Initialize() must run before any agent query");
    return *agent_manager_;
  }
  std::shared_ptr<Session> GetOrCreateDefaultSession();

  std::string HelpText();

  const config::AgentEntry& ActiveAgent() const;

  bool RewindBefore(size_t turn);
  void ClearConversation();

  config::BackendConfig CurrentBackend() const;

  ThinkingLevel CurrentThinkingLevel() const;
  bool SupportsThinkingLevel() const;
  std::optional<ThinkingLevel> GetThinkingOverride() const;
  bool SetThinkingLevel(std::optional<ThinkingLevel> level);

 private:
  config::BackendConfig ConfiguredBackend() const;
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

  std::vector<std::shared_ptr<mcp::McpClient>> mcp_clients_;
};

}  // namespace pu
