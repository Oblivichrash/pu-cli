// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>

#include "pu/agent_manager.hpp"
#include "pu/agent_config.hpp"
#include "pu/core/cancel_token.hpp"
#include "pu/executor.hpp"
#include "pu/mcp/mcp_client.hpp"
#include "pu/command_router.hpp"
#include "pu/session/session.hpp"
#include "pu/session/workspace.hpp"
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

  std::shared_ptr<Session> GetDefaultSession();

  ExecutionResult ProcessInput(const std::string& input, bool& is_command,
                               CancelToken cancel_token = nullptr,
                               std::function<void(const std::string&)> content_callback = nullptr,
                               ToolCallbacks tool_callbacks = {});

  void SetDefaultAgent(const std::string& agent_name);
  void SwitchAgent(const config::AgentEntry& new_agent);

  bool SwitchWorkspace(const std::filesystem::path& new_root);
  std::vector<std::pair<std::string, std::string>> ListWorkspaces() const;
  std::filesystem::path GetWorkspaceRoot() const { return workspace_root_; }
  std::string GetWorkspaceName() const { return workspace_root_.filename().string(); }

  AgentManager& GetAgentManager() { return *agent_manager_; }

 private:
  std::shared_ptr<Session> GetOrCreateDefaultSession();

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
