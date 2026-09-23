// SPDX-License-Identifier: GPL-3.0-only
#include "pu/runtime.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

#include <boost/json.hpp>
#include <spdlog/spdlog.h>
#include "pu/core/json.hpp"

#include "pu/agent.hpp"
#include "pu/core/logging.hpp"
#include "pu/core/base.hpp"
#include "pu/session/session.hpp"
#include "pu/tools/builtin_tools.hpp"
#include "pu/tools/mcp_tool.hpp"

namespace pu {
namespace {

std::filesystem::path ResolveWorkspacePath(const std::filesystem::path& root,
                                           const std::string& configured_path) {
  const auto path = configured_path.empty() ? std::filesystem::path(".")
                                            : std::filesystem::path(configured_path);
  return path.is_absolute() ? path : root / path;
}

std::shared_ptr<Session> LoadSessionFromFile(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path))
    return nullptr;
  std::ifstream file(path);
  if (!file.is_open())
    return nullptr;

  boost::json::value j;
  try {
    file >> j;
  } catch (const std::exception& e) {
    spdlog::error("Session file {} could not be parsed: {}", path.string(), e.what());
    return nullptr;
  }

  auto session = Session::Deserialize(j);
  if (session) return session;

  // The format changed incompatibly, so the file is reported rather than
  // guessed at. The backup is the copy that still holds the original.
  const std::string reason =
      json::HasKey(j, "schema_version")
          ? "schema_version " + std::to_string(
                json::ValueOrDefault<int>(j, "schema_version", 0)) +
                " without DAG node storage"
          : "missing schema_version";
  const std::filesystem::path backup =
      path.parent_path() / "session.v1.backup.json";

  std::ostringstream message;
  message << "session.json uses the pre-DAG (v1) layout and cannot be loaded.\n"
          << "  file:    " << path.string() << "\n"
          << "  reason:  " << reason << "\n";
  if (std::filesystem::exists(backup)) {
    message << "  backup:  " << backup.string() << "\n";
  }
  message << "\nThe original conversation is preserved in the backup file only. "
          << "The main file is overwritten on the next save.";
  spdlog::error("{}", message.str());
  return nullptr;
}

// The v1 session layout cannot be read by the context DAG format, so keep one
// copy of it before the schema upgrade ships. Never overwrites an existing
// backup, otherwise the original pre-upgrade state would be lost on the second
// run.
void BackupLegacySession(const std::filesystem::path& session_path) {
  const auto backup_path = session_path.parent_path() / "session.v1.backup.json";
  if (!std::filesystem::exists(session_path) || std::filesystem::exists(backup_path))
    return;

  std::error_code ec;
  std::filesystem::copy_file(session_path, backup_path,
                             std::filesystem::copy_options::none, ec);
  if (ec) {
    spdlog::warn("Failed to back up legacy session to {}: {}", backup_path.string(),
                 ec.message());
    return;
  }
  spdlog::info("Backed up legacy session to {}", backup_path.string());
}

}  // namespace

void Runtime::Initialize(const std::string& config_path) {
  if (is_initialized_)
    return;

  if (workspace_root_.empty())
    workspace_root_ = std::filesystem::current_path();

  std::string log_level = std::getenv("PU_LOG_LEVEL") ? std::getenv("PU_LOG_LEVEL") : "";
  pu::InitLogging(log_level);

  std::string cfg_path = config_path.empty()
      ? (workspace_root_ / ".pu" / "agents.json").string()
      : config_path;

  if (!std::filesystem::exists(cfg_path)) {
    cfg_path = config::FindConfigPath();
  }

  auto agents_cfg = config::LoadAgentsConfig(cfg_path);

  const std::string active_agent = default_agent_override_.empty()
      ? agents_cfg.default_agent
      : default_agent_override_;
  const auto default_entry = std::find_if(
      agents_cfg.agents.begin(), agents_cfg.agents.end(), [&](const config::AgentEntry& entry) {
        return entry.name == active_agent;
      });
  if (default_entry == agents_cfg.agents.end())
    throw Error("Requested agent is not configured: " + active_agent);

  agent_manager_ = std::make_unique<AgentManager>();
  agent_manager_->SetActiveAgent(active_agent);
  agent_manager_->LoadAgentConfigs(agents_cfg.agents);

  command_router_ = std::make_unique<CommandRouter>(*agent_manager_, *this);

  executor_ = std::make_unique<Executor>(nullptr);

  RebuildToolbox(*default_entry);

  auto session_path = workspace_root_ / ".pu" / "session.json";
  BackupLegacySession(session_path);
  if (std::filesystem::exists(session_path)) {
    // A refused file has already been reported with its reason and backup, so a
    // second, vaguer line here would only add noise.
    current_session_ = LoadSessionFromFile(session_path);
  }

  // The session names the agent it was talking to, and that name wins over the
  // configured default: otherwise the toolbox would describe one agent while the
  // provider talks to another.
  if (current_session_) {
    auto& spec = current_session_->GetRuntimeSpec();
    const auto* stored = agent_manager_->GetAgentConfig(spec.agent_name);
    if (stored != nullptr) {
      RebuildToolbox(*stored);
    } else {
      spdlog::warn("Session names an agent that is not configured: {}", spec.agent_name);
      spec.agent_name = active_agent;
    }
  }

  is_initialized_ = true;
  is_running_ = true;
}

void Runtime::Shutdown() {
  SaveCurrentSession();
  is_running_ = false;
}

void Runtime::SaveCurrentSession() {
  if (!current_session_)
    return;
  auto path = workspace_root_ / ".pu" / "session.json";
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  if (file.is_open()) {
    auto j = current_session_->Serialize();
    file << json::PrettyPrint(j);
  } else {
    spdlog::warn("Failed to write session file: {}", path.string());
  }
}

std::shared_ptr<Session> Runtime::GetOrCreateDefaultSession() {
  if (current_session_)
    return current_session_;

  auto session = std::make_shared<Session>();
  const auto active_agent = agent_manager_->GetActiveAgent();
  if (!agent_manager_->GetAgentConfig(active_agent))
    throw Error("Active agent is not configured: " + active_agent);
  session->SetAgent(active_agent);

  current_session_ = session;
  return current_session_;
}

config::BackendConfig Runtime::CurrentBackend() const {
  if (!agent_manager_) throw Error("Runtime is not initialized");

  if (current_session_) {
    const auto& spec = current_session_->GetRuntimeSpec();
    if (spec.backend_override) return *spec.backend_override;

    const auto* agent = agent_manager_->GetAgentConfig(spec.agent_name);
    if (agent == nullptr) throw Error("Active agent is not configured: " + spec.agent_name);
    return agent->backend;
  }

  const auto* agent = agent_manager_->GetAgentConfig(agent_manager_->GetActiveAgent());
  if (agent == nullptr) throw Error("Active agent is not configured");
  return agent->backend;
}

ExecutionResult Runtime::ProcessInput(const std::string& input,
                                      bool& is_command,
                                      CancelToken cancel_token,
                                      std::function<void(const std::string&)> content_callback,
                                      ToolCallbacks tool_callbacks) {
  ExecutionResult result;
  try {
    BeginRequest();

    if (!is_running_) {
      result.has_error = true;
      result.error_message = "Runtime is not running.";
      return result;
    }

    auto session = GetOrCreateDefaultSession();

    if (!input.empty() && input[0] == '/') {
      is_command = true;
      std::string output;
      bool ok = command_router_->Route(input, *session, output);
      result.content = output;
      result.was_streamed = false;
      result.has_error = !ok;
      if (!ok)
        result.error_message = output;
      SaveCurrentSession();
      return result;
    }

    is_command = false;

    auto provider = session->CreateProvider(CurrentBackend());
    auto exec_result = executor_->Execute(input, session->GetWorkspace(), provider.get(),
                                          cancel_token, content_callback, tool_callbacks);
    result = std::move(exec_result);
    SaveCurrentSession();
    return result;
  } catch (const std::exception& e) {
    result.has_error = true;
    result.error_message = e.what();
    return result;
  }
}

bool Runtime::SwitchWorkspace(const std::filesystem::path& new_root) {
  if (new_root == workspace_root_)
    return true;

  if (!std::filesystem::exists(new_root / ".pu" / "agents.json")) {
    spdlog::error("No agents.json found in {}", new_root.string());
    return false;
  }

  SaveCurrentSession();

  current_session_.reset();
  ShutdownMCP();
  toolbox_.reset();
  executor_.reset();
  agent_manager_.reset();
  command_router_.reset();

  workspace_root_ = std::filesystem::absolute(new_root);

  is_initialized_ = false;
  Initialize("");
  return true;
}

std::vector<std::pair<std::string, std::string>> Runtime::ListWorkspaces() const {
  std::vector<std::pair<std::string, std::string>> workspaces;
  const auto parent = workspace_root_.parent_path();
  for (const auto& entry : std::filesystem::directory_iterator(parent)) {
    if (entry.is_directory()) {
      auto agents_path = entry.path() / ".pu" / "agents.json";
      if (std::filesystem::exists(agents_path)) {
        workspaces.emplace_back(entry.path().filename().string(),
                                std::filesystem::absolute(entry.path()).string());
      }
    }
  }
  return workspaces;
}

void Runtime::SetDefaultAgent(const std::string& agent_name) {
  default_agent_override_ = agent_name;
}

void Runtime::ShutdownMCP() {
  for (auto& client : mcp_clients_) {
    if (client)
      client->Disconnect();
  }
  mcp_clients_.clear();
}

bool Runtime::StartMCP(const pu::mcp::McpServerConfig& config) {
  auto client = std::make_unique<mcp::McpClient>(config);
  if (client->Connect()) {
    mcp_clients_.push_back(std::move(client));
    return true;
  }
  spdlog::warn("MCP server '{}' connection failed", config.name);
  return false;
}

void Runtime::RegisterBuiltinTools(const config::AgentEntry& agent) {
  toolbox_->RegisterTool(std::make_unique<tools::ExecuteBashToolStandard>(
  ResolveWorkspacePath(workspace_root_, agent.security.sandbox_root).string()));
  toolbox_->RegisterTool(std::make_unique<tools::WriteFileTool>());
  toolbox_->RegisterTool(std::make_unique<tools::AskUserTool>());
}

void Runtime::RebuildToolbox(const config::AgentEntry& agent) {
  ShutdownMCP();

  toolbox_ = std::make_unique<Toolbox>();
  RegisterBuiltinTools(agent);

  for (const auto& mcp_cfg : agent.mcp_servers) {
    if (!StartMCP(mcp_cfg)) {
      spdlog::warn("Skipping MCP server '{}' - connection failed", mcp_cfg.name);
      continue;
    }

    auto* client = mcp_clients_.back().get();
    auto tools = client->ListTools();
    for (const auto& t : tools) {
      auto mcp_tool = std::make_unique<tools::McpTool>(client, t, mcp_cfg.name);
      toolbox_->RegisterTool(std::move(mcp_tool));
      spdlog::debug("Registered MCP tool: mcp.{}.{}", mcp_cfg.name, t.name);
    }
  }

  auto security = agent.security;
  security.sandbox_root = ResolveWorkspacePath(workspace_root_, security.sandbox_root).string();
  executor_->SetSecurityPolicy(std::move(security));
  executor_->SetToolbox(toolbox_.get());
  executor_->SetSystemPrompt(agent.backend.system_prompt.value_or(""));
  agent_manager_->SetActiveAgent(agent.name);
}

void Runtime::SwitchAgent(const config::AgentEntry& new_agent) {
  if (agent_manager_->GetActiveAgent() == new_agent.name)
    return;
  RebuildToolbox(new_agent);

  if (current_session_) {
    try {
      current_session_->SetAgent(new_agent.name);
    } catch (const std::exception& e) {
      spdlog::warn("Failed to sync session config: {}", e.what());
    }
  }
}

}  // namespace pu
