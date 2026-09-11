// SPDX-License-Identifier: GPL-3.0-only
#include "pu/runtime.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

#include <boost/json.hpp>
#include <spdlog/spdlog.h>
#include "pu/core/json.hpp"

#include "pu/agent_config.hpp"
#include "pu/core/logging.hpp"
#include "pu/core/error.hpp"
#include "pu/core/path_utils.hpp"
#include "pu/session/workspace.hpp"
#include "pu/tools/builtin_tools.hpp"
#include "pu/tools/mcp_tool.hpp"

namespace pu {
namespace {

std::shared_ptr<Session> LoadSessionFromFile(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path))
    return nullptr;
  std::ifstream file(path);
  if (!file.is_open())
    return nullptr;
  try {
    boost::json::value j;
    file >> j;
    return Session::Deserialize(j);
  } catch (const std::exception& e) {
    spdlog::warn("Failed to parse session from {}: {}", path.string(), e.what());
    return nullptr;
  }
}

}  // namespace

void Runtime::Initialize(const std::string& config_path) {
  if (is_initialized_)
    return;

  if (workspace_root_.empty())
    workspace_root_ = std::filesystem::current_path();

  std::string log_level = std::getenv("PU_LOG_LEVEL") ? std::getenv("PU_LOG_LEVEL") : "";
  bool trace = std::getenv("PU_TRACE") && std::string(std::getenv("PU_TRACE")) == "1";
  pu::InitLogging(log_level, trace);

  std::string cfg_path = config_path.empty()
      ? (workspace_root_ / ".pu" / "agents.json").string()
      : config_path;

  if (!std::filesystem::exists(cfg_path)) {
    cfg_path = config::FindConfigPath();
  }

  auto agents_cfg = config::LoadAgentsConfig(cfg_path);

  agent_manager_ = std::make_unique<AgentManager>();
  agent_manager_->SetActiveAgent(agents_cfg.default_agent);
  agent_manager_->LoadAgentConfigs(agents_cfg.agents);

  const config::AgentEntry* default_entry = nullptr;
  for (const auto& entry : agents_cfg.agents) {
    if (entry.name == agents_cfg.default_agent) {
      default_entry = &entry;
      break;
    }
  }
  if (default_entry) {
    default_backend_config_ = default_entry->backend;
  }

  command_router_ = std::make_unique<CommandRouter>(*agent_manager_, *this);

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
    executor_->SetToolbox(toolbox_.get());
    spdlog::warn("No default agent found. Using permissive fallback.");
  }

  auto session_path = workspace_root_ / ".pu" / "session.json";
  if (std::filesystem::exists(session_path)) {
    current_session_ = LoadSessionFromFile(session_path);
    if (!current_session_) {
      spdlog::warn("Failed to load session from {}", session_path.string());
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

std::shared_ptr<Session> Runtime::GetDefaultSession() {
  return GetOrCreateDefaultSession();
}

std::shared_ptr<Session> Runtime::GetOrCreateDefaultSession() {
  if (current_session_)
    return current_session_;

  auto session = std::make_shared<Session>();

  std::string active_agent =
      default_agent_override_.empty() ? agent_manager_->GetActiveAgent() : default_agent_override_;
  if (active_agent.empty())
    active_agent = "chat";
  session->SwitchAgent(active_agent);
  session->SwitchBackend(default_backend_config_);

  current_session_ = session;
  return current_session_;
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
    if (!session) {
      result.has_error = true;
      result.error_message = "Session not found.";
      return result;
    }

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

    auto provider = session->CreateProvider();
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

  workspace_root_ = new_root;
  std::filesystem::current_path(workspace_root_);

  is_initialized_ = false;
  Initialize("");
  return true;
}

std::vector<std::pair<std::string, std::string>> Runtime::ListWorkspaces() const {
  std::vector<std::pair<std::string, std::string>> workspaces;
  for (const auto& entry : std::filesystem::directory_iterator(".")) {
    if (entry.is_directory()) {
      auto agents_path = entry.path() / ".pu" / "agents.json";
      if (std::filesystem::exists(agents_path)) {
        workspaces.emplace_back(entry.path().filename().string(), entry.path().string());
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

void Runtime::RegisterBuiltinTools() {
  toolbox_->RegisterTool(std::make_unique<tools::ExecuteBashToolStandard>(
      current_agent_config_.security.sandbox_root.empty() ? "."
                                                          : current_agent_config_.security
                                                                .sandbox_root));
  toolbox_->RegisterTool(std::make_unique<tools::WriteFileTool>());
  toolbox_->RegisterTool(std::make_unique<tools::AskUserTool>());
}

void Runtime::RebuildToolbox(const config::AgentEntry& agent) {
  ShutdownMCP();

  toolbox_ = std::make_unique<Toolbox>();
  RegisterBuiltinTools();

  for (const auto& mcp_cfg : agent.mcp_servers) {
    if (!StartMCP(mcp_cfg)) {
      spdlog::warn("Skipping MCP server '{}' — connection failed", mcp_cfg.name);
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

  executor_->SetSecurityPolicy(agent.security);
  executor_->SetToolbox(toolbox_.get());
  executor_->SetCompactionConfig(agent.compaction);
  agent_manager_->SetActiveAgent(agent.name);
}

void Runtime::SwitchAgent(const config::AgentEntry& new_agent) {
  if (current_agent_name_ == new_agent.name)
    return;
  current_agent_config_ = new_agent;
  current_agent_name_ = new_agent.name;
  RebuildToolbox(new_agent);

  if (current_session_) {
    try {
      current_session_->SwitchBackend(new_agent.backend);
      current_session_->SwitchAgent(new_agent.name);
    } catch (const std::exception& e) {
      spdlog::warn("Failed to sync session config: {}", e.what());
    }
  }
}

}  // namespace pu
