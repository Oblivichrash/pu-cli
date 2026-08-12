// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pu/agent_manager.hpp"
#include "pu/agent_config.hpp"
#include "pu/executor.hpp"
#include "pu/mcp/mcp_client.hpp"
#include "pu/command_router.hpp"
#include "pu/session/session.hpp"
#include "pu/session/workspace.hpp"
#include "pu/session_store.hpp"
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

  std::string CreateSession(const std::string& owner_id,
                            const std::string& agent_name = "",
                            const config::BackendConfig* backend = nullptr);
  std::shared_ptr<Session> GetSession(const std::string& id);
  std::vector<std::string> ListSessions() const;
  bool DestroySession(const std::string& id);

  // Single store instance shared by Runtime autosave and the /save,/load,/list
  // command handlers, so all persistence goes through one directory.
  SessionStore& GetSessionStore();

  std::shared_ptr<Session> GetDefaultSession();

  bool ProcessInput(const std::string& session_id, const std::string& input,
                    ExecutionResult& result, bool& is_command);

  void SetDefaultAgent(const std::string& agent_name);
  void SwitchAgent(const config::AgentEntry& new_agent);

 private:
  std::shared_ptr<Session> GetOrCreateDefaultSession();

  void RebuildToolbox(const config::AgentEntry& agent);
  void ShutdownMCP();
  bool StartMCP(const pu::mcp::McpServerConfig& config);
  void RegisterBuiltinTools(const std::vector<std::string>& allowed_tools);

  bool is_initialized_ = false;
  bool is_running_ = false;
  std::unique_ptr<AgentManager> agent_manager_;
  std::unique_ptr<SessionStore> session_store_;
  std::unique_ptr<CommandRouter> command_router_;
  std::unique_ptr<Toolbox> toolbox_;
  std::unique_ptr<Executor> executor_;
  std::map<std::string, std::shared_ptr<Session>> sessions_;
  std::string default_session_id_;
  config::BackendConfig default_backend_config_;

  std::string default_agent_override_;

  std::vector<std::unique_ptr<mcp::McpClient>> mcp_clients_;
  std::string current_agent_name_;
  config::AgentEntry current_agent_config_;
};

}  // namespace pu