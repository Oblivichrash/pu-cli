// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>
#include "pu/session/session.hpp"
#include "pu/storage/session_store.hpp"
#include "pu/agent/agent_manager.hpp"
#include "pu/runtime/command_router.hpp"
#include "pu/executor/executor.hpp"
#include "pu/tools/toolbox.hpp"
#include "pu/agent_config.hpp"
#include "pu/mcp/mcp_client.hpp"

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
                            const BackendConfig* backend = nullptr);
  std::shared_ptr<Session> GetSession(const std::string& id);
  std::vector<std::string> ListSessions() const;
  bool DestroySession(const std::string& id);

  std::shared_ptr<Session> GetDefaultSession();
  void SetDefaultSessionId(const std::string& id);

  bool ProcessInput(const std::string& session_id,
                    const std::string& input,
                    std::string& output,
                    bool& is_command);

  void SetMaxSessions(int n);
  void SetMaxDepth(int depth);

  // B.2: Reload external tools
  void ReloadTools();

  // B.3: Override default agent name for startup
  void SetDefaultAgent(const std::string& agent_name);

  // Stage 6+7: Switch the active agent and rebuild the tool registry.
  void SwitchAgent(const config::AgentEntry& new_agent);

private:
  std::shared_ptr<Session> GetOrCreateDefaultSession();
  void AutoSaveSession(const std::string& id);

  // Stage 6+7: tool lifecycle helpers
  void RebuildToolbox(const config::AgentEntry& agent);
  void ShutdownMCP();
  bool StartMCP(const pu::mcp::McpServerConfig& config);
  void RegisterBuiltinTools();
  void RegisterPythonTools();

  bool is_initialized_ = false;
  bool is_running_ = false;
  std::unique_ptr<AgentManager> agent_manager_;
  std::unique_ptr<SessionStore> session_store_;
  std::unique_ptr<CommandRouter> command_router_;
  std::unique_ptr<Toolbox> toolbox_;
  std::unique_ptr<Executor> executor_;
  std::map<std::string, std::shared_ptr<Session>> sessions_;
  std::string default_session_id_;
  BackendConfig default_backend_config_;
  int max_sessions_ = 10;
  int max_depth_ = 5;

  // B.3: default agent override (set via --agent flag)
  std::string default_agent_override_;

  // Stage 6+7: per-agent tool registry state. The Runtime is the sole owner of
  // the Toolbox; it is rebuilt whenever the active agent changes.
  std::unique_ptr<mcp::McpClient> current_mcp_client_;
  std::string current_agent_name_;
  config::AgentEntry current_agent_config_;
};

} // namespace pu