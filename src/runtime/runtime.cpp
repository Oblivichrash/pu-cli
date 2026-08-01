// SPDX-License-Identifier: GPL-3.0-only
#include "pu/runtime/runtime.hpp"

#include <filesystem>
#include <iostream>

#include <spdlog/spdlog.h>

#include "infra/curl_http_client.hpp"
#include "pu/agent_config.hpp"
#include "pu/core/fork_merge_service.hpp"
#include "pu/core/logging.hpp"
#include "pu/error.hpp"
#include "pu/path_utils.hpp"
#include "pu/session/workspace.hpp"
#include "pu/tools/create_tool.hpp"
#include "pu/tools/execute_bash_tool.hpp"
#include "pu/tools/fork_tools.hpp"
#include "pu/tools/mcp_tool.hpp"
#include "pu/tools/write_file_tool.hpp"
#include "tools/command_executor.hpp"

namespace pu {

void Runtime::Initialize(const std::string& config_path) {
  if (is_initialized_) return;

  std::string log_level = std::getenv("PU_LOG_LEVEL") ? std::getenv("PU_LOG_LEVEL") : "";
  bool trace = std::getenv("PU_TRACE") && std::string(std::getenv("PU_TRACE")) == "1";
  pu::InitLogging(log_level, trace);

  auto cfg_path = config_path.empty() ? config::FindConfigPath() : config_path;
  auto agents_cfg = config::LoadAgentsConfig(cfg_path);

  agent_manager_ = std::make_unique<AgentManager>();
  agent_manager_->SetActiveAgent(agents_cfg.default_agent);
  agent_manager_->LoadAgentConfigs(agents_cfg.agents);

  // Create the shared root workspace and ForkMergeService once.
  auto root_ws = std::make_shared<Workspace>("root");
  fork_service_ = std::make_shared<ForkMergeService>(*agent_manager_, root_ws);

  const config::AgentEntry* default_entry = nullptr;
  for (const auto& entry : agents_cfg.agents) {
    if (entry.name == agents_cfg.default_agent) {
      default_entry = &entry;
      break;
    }
  }
  if (default_entry) {
    default_backend_config_.type =
        (default_entry->backend.type == config::BackendType::kOllama) ? "ollama" : "openai";
    default_backend_config_.host = default_entry->backend.host;
    default_backend_config_.model = default_entry->backend.model;
    default_backend_config_.api_key = default_entry->backend.api_key.value_or("");
    default_backend_config_.temperature = default_entry->backend.temperature;
  }

  session_store_ = std::make_unique<SessionStore>();
  command_router_ = std::make_unique<CommandRouter>(*agent_manager_, *this, fork_service_);

  executor_ = std::make_unique<Executor>(nullptr);

  if (default_entry) {
    current_agent_config_ = *default_entry;
    current_agent_name_ = default_entry->name;
    RebuildToolbox(*default_entry);
  } else {
    config::SecurityPolicy fallback_policy;
    fallback_policy.sandbox_root = ".";
    fallback_policy.max_command_length = 0;
    fallback_policy.forbidden_patterns = {};
    executor_->SetSecurityPolicy(fallback_policy);
    toolbox_ = std::make_unique<Toolbox>();
    RegisterBuiltinTools();
    RegisterPythonTools();
    executor_->SetToolbox(toolbox_.get());
    spdlog::warn("No default agent found for security policy. Using permissive fallback.");
  }

  auto metadata = session_store_->ListAllMetadata();
  if (!metadata.empty()) {
    default_session_id_ = metadata[0].id;
  }

  is_initialized_ = true;
  is_running_ = true;
}

void Runtime::Shutdown() {
  if (!is_initialized_) return;
  for (const auto& [id, session] : sessions_) {
    session_store_->SaveSession(*session);
  }
  is_running_ = false;
}

std::string Runtime::CreateSession(const std::string& owner_id,
                                   const std::string& agent_name,
                                   const SessionBackendConfig* backend) {
  if (sessions_.size() >= static_cast<size_t>(max_sessions_)) {
    throw RuntimeError("Maximum sessions reached");
  }

  auto id = "session_" +
            std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
  auto session = std::make_shared<Session>(id, owner_id);

  std::string active_agent = agent_name.empty() ? agent_manager_->GetActiveAgent() : agent_name;
  if (active_agent.empty()) active_agent = "chat";
  session->SwitchAgent(active_agent);

  if (backend) {
    session->SwitchBackend(*backend);
  } else {
    session->SwitchBackend(default_backend_config_);
  }

  sessions_[id] = session;
  session_store_->SaveSession(*session);
  return id;
}

std::shared_ptr<Session> Runtime::GetSession(const std::string& id) {
  auto it = sessions_.find(id);
  if (it != sessions_.end()) {
    it->second->Touch();
    return it->second;
  }

  auto session = session_store_->LoadSession(id);
  if (session) {
    if (session->GetRuntimeSpec().backend.type.empty()) {
      session->SwitchBackend(default_backend_config_);
      session_store_->SaveSession(*session);
    }
    sessions_[id] = std::move(session);
    sessions_[id]->Touch();
    return sessions_[id];
  }
  return nullptr;
}

std::vector<std::string> Runtime::ListSessions() const {
  std::vector<std::string> ids;
  for (const auto& [id, _] : sessions_) ids.push_back(id);
  auto metadata = session_store_->ListAllMetadata();
  for (const auto& meta : metadata) {
    if (sessions_.find(meta.id) == sessions_.end()) {
      ids.push_back(meta.id);
    }
  }
  return ids;
}

bool Runtime::DestroySession(const std::string& id) {
  if (id == default_session_id_) return false;
  sessions_.erase(id);
  session_store_->DeleteSession(id);
  return true;
}

std::shared_ptr<Session> Runtime::GetDefaultSession() {
  if (!default_session_id_.empty()) {
    auto session = GetSession(default_session_id_);
    if (session) return session;
  }
  return GetOrCreateDefaultSession();
}

std::shared_ptr<Session> Runtime::GetOrCreateDefaultSession() {
  auto owner = getenv("USER") ? getenv("USER") : "default";
  std::string agent = default_agent_override_.empty() ? agent_manager_->GetActiveAgent()
                                                      : default_agent_override_;
  auto id = CreateSession(owner, agent);
  default_session_id_ = id;
  return GetSession(id);
}

bool Runtime::ProcessInput(const std::string& session_id,
                           const std::string& input,
                           ExecutionResult& result,
                           bool& is_command) {
  if (!is_running_) {
    result.has_error = true;
    result.error_message = "Runtime is not running.";
    return false;
  }

  auto session = session_id.empty() ? GetDefaultSession() : GetSession(session_id);
  if (!session) {
    result.has_error = true;
    result.error_message = "Session not found.";
    return false;
  }

  if (!input.empty() && input[0] == '/') {
    is_command = true;
    std::string output;
    bool ok = command_router_->Route(input, *session, output);
    result.content = output;
    result.was_streamed = false;
    result.has_error = !ok;
    if (!ok) result.error_message = output;
    return ok;
  }

  is_command = false;

  // Ensure the session's call stack has a root workspace that matches
  // the shared ForkMergeService root.
  auto call_stack_ptr = session->GetCallStackPtr();
  if (!call_stack_ptr->GetRootWorkspace()) {
    call_stack_ptr->SetRootWorkspace(fork_service_->GetRootWorkspace());
  }

  // Share the single ForkMergeService and the session's CallStack with the executor.
  executor_->SetForkService(fork_service_);
  executor_->SetCallStack(call_stack_ptr);

  auto provider = session->CreateProvider();
  auto exec_result = executor_->Execute(input, session->GetWorkspace(), provider.get());
  result = std::move(exec_result);
  session_store_->SaveSession(*session);
  return true;
}

void Runtime::ReloadTools() {
  auto tools_dir = pu::path::GetDataDir() / "tools";
  if (toolbox_) {
    toolbox_->ReloadExternalTools(tools_dir.string());
  }
}

void Runtime::SetDefaultAgent(const std::string& agent_name) {
  default_agent_override_ = agent_name;
}

void Runtime::ShutdownMCP() {
  if (current_mcp_client_) {
    current_mcp_client_->Disconnect();
    current_mcp_client_.reset();
  }
}

bool Runtime::StartMCP(const pu::mcp::McpServerConfig& config) {
  current_mcp_client_ = std::make_unique<mcp::McpClient>(config);
  if (current_mcp_client_->Connect()) {
    return true;
  } else {
    spdlog::warn("MCP server '{}' connection failed", config.name);
    current_mcp_client_.reset();
    return false;
  }
}

void Runtime::RegisterBuiltinTools() {
  toolbox_->RegisterTool(std::make_unique<tools::ExecuteBashToolStandard>(
      std::make_unique<executor::CommandExecutor>(
          current_agent_config_.security.sandbox_root.empty() ? "."
                                                              : current_agent_config_.security
                                                                    .sandbox_root)));
  toolbox_->RegisterTool(std::make_unique<tools::WriteFileTool>());
  toolbox_->RegisterTool(std::make_unique<tools::CreateTool>());
  toolbox_->RegisterTool(std::make_unique<tools::ForkWorkspaceTool>());
  toolbox_->RegisterTool(std::make_unique<tools::MergeWorkspaceTool>());
  toolbox_->RegisterTool(std::make_unique<tools::ListForksTool>());
}

void Runtime::RegisterPythonTools() {
  auto tools_dir = pu::path::GetDataDir() / "tools";
  if (std::filesystem::exists(tools_dir)) {
    toolbox_->ReloadExternalTools(tools_dir.string());
  }
}

void Runtime::RebuildToolbox(const config::AgentEntry& agent) {
  ShutdownMCP();

  toolbox_ = std::make_unique<Toolbox>();

  RegisterBuiltinTools();
  RegisterPythonTools();

  if (!agent.mcp_servers.empty()) {
    const auto& mcp_cfg = agent.mcp_servers[0];
    if (StartMCP(mcp_cfg)) {
      auto tools = current_mcp_client_->ListTools();
      for (const auto& t : tools) {
        auto mcp_tool = std::make_unique<tools::McpTool>(current_mcp_client_.get(), t);
        toolbox_->RegisterTool(std::move(mcp_tool));
        spdlog::debug("Registered MCP tool: mcp.{} from server {}", t.name, mcp_cfg.name);
      }
    }
  }

  executor_->SetSecurityPolicy(agent.security);
  executor_->SetToolbox(toolbox_.get());
  executor_->SetCompactionConfig(agent.compaction);
  agent_manager_->SetActiveAgent(agent.name);
}

void Runtime::SwitchAgent(const config::AgentEntry& new_agent) {
  if (current_agent_name_ == new_agent.name) return;
  current_agent_config_ = new_agent;
  current_agent_name_ = new_agent.name;
  RebuildToolbox(new_agent);

  if (!default_session_id_.empty()) {
    auto session = GetSession(default_session_id_);
    if (session) {
      try {
        session->SwitchAgent(new_agent.name);
      } catch (const std::exception& e) {
        spdlog::warn("Failed to sync default session agent name: {}", e.what());
      }
    }
  }
}

}  // namespace pu
