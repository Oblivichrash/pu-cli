// SPDX-License-Identifier: GPL-3.0-only
#include "pu/runtime.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

#include <spdlog/spdlog.h>

#include "infra/curl_http_client.hpp"
#include "pu/agent_config.hpp"
#include "pu/core/logging.hpp"
#include "pu/error.hpp"
#include "pu/path_utils.hpp"
#include "pu/session/workspace.hpp"
#include "pu/tools/builtin_tools.hpp"
#include "pu/tools/mcp_tool.hpp"

namespace pu {

namespace {

std::filesystem::path SessionFilePath() {
  return pu::path::GetDataDir() / "session.json";
}

void SaveCurrentSession(const std::shared_ptr<Session>& session) {
  if (!session) return;
  auto j = session->Serialize();
  auto path = SessionFilePath();
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  if (file.is_open()) {
    file << j.dump(2);
  } else {
    spdlog::warn("Failed to write session file: {}", path.string());
  }
}

}  // namespace

void Runtime::Initialize(const std::string& config_path) {
  if (is_initialized_) return;

  std::string log_level = std::getenv("PU_LOG_LEVEL") ? std::getenv("PU_LOG_LEVEL") : "";
  bool trace = std::getenv("PU_TRACE") && std::string(std::getenv("PU_TRACE")) == "1";
  pu::InitLogging(log_level, trace);

  // Ensure the data directory layout exists before any session/log access.
  std::filesystem::create_directories(pu::path::GetDataDir() / "logs");

  auto cfg_path = config_path.empty() ? config::FindConfigPath() : config_path;
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
    spdlog::warn("No default agent found for security policy. Using permissive fallback.");
  }

  // Automatically load the single session from session.json if present.
  auto path = SessionFilePath();
  if (std::filesystem::exists(path)) {
    std::ifstream file(path);
    if (file.is_open()) {
      nlohmann::json j;
      try {
        file >> j;
        current_session_ = Session::Deserialize(j);
      } catch (const std::exception& e) {
        spdlog::warn("Failed to load session from {}: {}", path.string(), e.what());
        current_session_.reset();
      }
    }
  }

  is_initialized_ = true;
  is_running_ = true;
}

void Runtime::Shutdown() {
  if (!is_initialized_) return;
  SaveCurrentSession(current_session_);
  is_running_ = false;
}

std::shared_ptr<Session> Runtime::GetDefaultSession() {
  return GetOrCreateDefaultSession();
}

std::shared_ptr<Session> Runtime::GetOrCreateDefaultSession() {
  if (current_session_) return current_session_;

  auto session = std::make_shared<Session>();

  std::string active_agent =
      default_agent_override_.empty() ? agent_manager_->GetActiveAgent() : default_agent_override_;
  if (active_agent.empty()) active_agent = "chat";
  session->SwitchAgent(active_agent);
  session->SwitchBackend(default_backend_config_);

  current_session_ = session;
  return current_session_;
}

bool Runtime::ProcessInput(const std::string& input,
                           ExecutionResult& result,
                           bool& is_command,
                           CancelToken cancel_token,
                           std::function<void(const std::string&)> content_callback) {
  BeginRequest();

  if (!is_running_) {
    result.has_error = true;
    result.error_message = "Runtime is not running.";
    return false;
  }

  auto session = GetOrCreateDefaultSession();
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
    SaveCurrentSession(session);
    return ok;
  }

  is_command = false;

  auto provider = session->CreateProvider();
  auto exec_result = executor_->Execute(input, session->GetWorkspace(), provider.get(),
                                        cancel_token, content_callback);
  result = std::move(exec_result);
  SaveCurrentSession(session);
  return true;
}

void Runtime::SetDefaultAgent(const std::string& agent_name) {
  default_agent_override_ = agent_name;
}

void Runtime::ShutdownMCP() {
  for (auto& client : mcp_clients_) {
    if (client) {
      client->Disconnect();
    }
  }
  mcp_clients_.clear();
}

bool Runtime::StartMCP(const pu::mcp::McpServerConfig& config) {
  auto client = std::make_unique<mcp::McpClient>(config);
  if (client->Connect()) {
    mcp_clients_.push_back(std::move(client));
    return true;
  } else {
    spdlog::warn("MCP server '{}' connection failed", config.name);
    return false;
  }
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

    // The last client pushed by StartMCP is the one we just connected.
    auto* client = mcp_clients_.back().get();
    auto tools = client->ListTools();
    for (const auto& t : tools) {
      auto mcp_tool = std::make_unique<tools::McpTool>(client, t, mcp_cfg.name);
      toolbox_->RegisterTool(std::move(mcp_tool));
      spdlog::debug("Registered MCP tool: mcp.{}.{} from server {}",
                    mcp_cfg.name, t.name, mcp_cfg.name);
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

  if (current_session_) {
    try {
      current_session_->SwitchAgent(new_agent.name);
    } catch (const std::exception& e) {
      spdlog::warn("Failed to sync default session agent name: {}", e.what());
    }
  }
}

}  // namespace pu