// SPDX-License-Identifier: GPL-3.0-only
#include "pu/runtime.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>

#include <boost/json.hpp>
#include <spdlog/spdlog.h>
#include "pu/core/json.hpp"

#include "pu/agent_manager.hpp"
#include "pu/config/agents.hpp"
#include "pu/core/logging.hpp"
#include "pu/core/base.hpp"
#include "pu/core/beast_http_client.hpp"
#include "pu/session/session.hpp"
#include "pu/tools/builtin_tools.hpp"
#include "pu/tools/mcp_tool.hpp"

namespace pu {
namespace {

std::filesystem::path ResolveWorkspacePath(const std::filesystem::path& root,
                                           const std::string& configured_path) {
  const auto path =
      configured_path.empty() ? std::filesystem::path(".") : std::filesystem::path(configured_path);
  return path.is_absolute() ? path : root / path;
}

std::shared_ptr<Session> LoadSessionFromFile(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) return nullptr;
  std::ifstream file(path);
  if (!file.is_open()) return nullptr;

  boost::json::value j;
  try {
    file >> j;
  } catch (const std::exception& e) {
    spdlog::error("Session file {} could not be parsed: {}", path.string(), e.what());
    return nullptr;
  }

  auto session = Session::Deserialize(j);
  if (session) return session;

  std::string reason;
  if (!json::HasKey(j, "schema_version")) {
    reason = "missing schema_version";
  } else if (const int version = json::ValueOrDefault<int>(j, "schema_version", 0);
             version != kSessionSchemaVersion) {
    reason = "schema_version " + std::to_string(version) + ", this build reads " +
             std::to_string(kSessionSchemaVersion);
  } else {
    reason = "history is not node storage";
  }
  spdlog::error(
      "session.json cannot be loaded by this build, so a fresh conversation starts.\n"
      "  file:    {}\n"
      "  reason:  {}\n"
      "The file is replaced by the next save.",
      path.string(), reason);
  return nullptr;
}

}  // namespace

void Runtime::Initialize(const std::string& config_path) {
  if (is_initialized_) return;

  if (workspace_root_.empty()) workspace_root_ = std::filesystem::current_path();

  std::string log_level = std::getenv("PU_LOG_LEVEL") ? std::getenv("PU_LOG_LEVEL") : "";
  pu::InitLogging(log_level);

  std::string cfg_path =
      config_path.empty() ? (workspace_root_ / ".pu" / "agents.json").string() : config_path;

  if (!std::filesystem::exists(cfg_path)) {
    cfg_path = config::FindConfigPath();
  }
  if (cfg_path.empty()) {
    throw Error("Configuration file not found. Place agents.json in ./.pu/ or ~/.pu/.");
  }

  auto agents_cfg = config::LoadAgentsConfig(cfg_path);

  const std::string active_agent =
      default_agent_override_.empty() ? agents_cfg.default_agent : default_agent_override_;
  const auto default_entry =
      std::find_if(agents_cfg.agents.begin(), agents_cfg.agents.end(),
                   [&](const config::AgentEntry& entry) { return entry.name == active_agent; });
  if (default_entry == agents_cfg.agents.end()) {
    std::string known;
    for (const auto& entry : agents_cfg.agents) known += " " + entry.name;
    throw Error("Requested agent is not configured: " + active_agent +
                (known.empty() ? " (no agents are configured)" : "; configured:" + known));
  }

  agent_manager_ = std::make_unique<AgentManager>();
  agent_manager_->SetActiveAgent(active_agent);
  agent_manager_->LoadAgentConfigs(agents_cfg.agents);

  command_router_ = std::make_unique<CommandRouter>(*this);

  executor_ = std::make_unique<Executor>();
  http_client_ = std::make_shared<pu::http::BeastHttpClient>();

  RebuildToolbox(*default_entry);

  const auto session_path = workspace_root_ / ".pu" / "session.json";
  if (std::filesystem::exists(session_path)) {
    current_session_ = LoadSessionFromFile(session_path);
  }

  if (current_session_) {
    auto& spec = current_session_->GetSpec();
    const auto* stored = agent_manager_->GetAgentConfig(spec.agent_name);
    if (stored == nullptr) {
      spdlog::warn("Session names an agent that is not configured: {}", spec.agent_name);
      spec.agent_name = active_agent;
    } else if (stored->name != active_agent) {
      RebuildToolbox(*stored);
    }
  }

  is_initialized_ = true;
}

void Runtime::Shutdown() {
  SaveCurrentSession();
  mcp::DisconnectMcpServers(mcp_clients_);
}

void Runtime::SaveCurrentSession() {
  if (!current_session_) return;
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

bool Runtime::RewindBefore(size_t turn) {
  if (!GetOrCreateDefaultSession()->GetConversation().RewindBefore(turn)) return false;
  SaveCurrentSession();
  return true;
}

void Runtime::ClearConversation() {
  GetOrCreateDefaultSession()->GetConversation().ClearHistory();
  SaveCurrentSession();
}

std::shared_ptr<Session> Runtime::GetOrCreateDefaultSession() {
  if (current_session_) return current_session_;

  auto session = std::make_shared<Session>();
  session->SetAgent(agent_manager_->GetActiveAgent());

  current_session_ = session;
  return current_session_;
}

const config::AgentEntry& Runtime::ActiveAgent() const {
  return *agent_manager_->GetAgentConfig(agent_manager_->GetActiveAgent());
}

std::string Runtime::HelpText() {
  assert(command_router_ && "Initialize() must run before any help query");
  return command_router_->GetHelpText();
}

config::BackendConfig Runtime::ConfiguredBackend() const {
  if (current_session_) {
    const auto& spec = current_session_->GetSpec();
    if (const auto* configured = agent_manager_->GetAgentConfig(spec.agent_name)) {
      return configured->backend;
    }
  }

  return ActiveAgent().backend;
}

config::BackendConfig Runtime::CurrentBackend() const {
  config::BackendConfig backend = ConfiguredBackend();
  if (current_session_) {
    const auto& spec = current_session_->GetSpec();
    if (spec.thinking_override) backend.thinking = *spec.thinking_override;
  }
  return backend;
}

ThinkingLevel Runtime::CurrentThinkingLevel() const { return CurrentBackend().thinking; }

std::optional<ThinkingLevel> Runtime::GetThinkingOverride() const {
  if (!current_session_) return std::nullopt;
  return current_session_->GetSpec().thinking_override;
}

bool Runtime::SetThinkingLevel(std::optional<ThinkingLevel> level) {
  if (!SupportsThinkingLevel()) return false;
  GetOrCreateDefaultSession()->GetSpec().thinking_override = level;
  SaveCurrentSession();
  return true;
}

bool Runtime::SupportsThinkingLevel() const {
  return current_session_ && config::CarriesThinkingLevel(CurrentBackend().type);
}

ExecutionResult Runtime::ProcessInput(const std::string& input, bool& is_command,
                                      CancelToken cancel_token,
                                      std::function<void(const std::string&)> content_callback,
                                      ToolCallbacks tool_callbacks,
                                      std::function<void(const std::string&)> reasoning_callback) {
  ExecutionResult result;
  try {
    BeginRequest();

    auto session = GetOrCreateDefaultSession();

    if (!input.empty() && input[0] == '/') {
      is_command = true;
      std::string output;
      bool ok = command_router_->Route(input, *session, output);
      result.content = output;
      result.was_streamed = false;
      result.has_error = !ok;
      if (!ok) result.error_message = output;
      SaveCurrentSession();
      return result;
    }

    is_command = false;

    auto provider = session->CreateProvider(CurrentBackend(), http_client_);
    auto exec_result =
        executor_->Execute(input, session->GetConversation(), provider.get(), toolbox_,
                           cancel_token, content_callback, tool_callbacks, reasoning_callback);
    result = std::move(exec_result);
    SaveCurrentSession();
    return result;
  } catch (const std::exception& e) {
    result.has_error = true;
    result.error_message = e.what();
    return result;
  }
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

void Runtime::RegisterBuiltinTools(const config::AgentEntry& agent) {
  toolbox_.RegisterTool(std::make_unique<tools::ExecuteBashToolStandard>(
      ResolveWorkspacePath(workspace_root_, agent.security.sandbox_root).string()));
  toolbox_.RegisterTool(std::make_unique<tools::WriteFileTool>());
}

void Runtime::RegisterMcpTools() {
  for (std::size_t i = 0; i < mcp_clients_.size(); ++i) {
    auto client = mcp_clients_[i];
    const std::string& server_name = client->ServerName();

    std::vector<ToolDefinition> tools;
    try {
      tools = client->ListTools();
    } catch (const std::exception& e) {
      spdlog::warn("Skipping MCP server '{}' - {}", server_name, e.what());
      continue;
    }

    for (const auto& t : tools) {
      auto mcp_tool = std::make_unique<tools::McpTool>(client, t, server_name);
      if (!toolbox_.RegisterTool(std::move(mcp_tool))) {
        spdlog::warn("Skipping MCP tool '{}' from '{}' - the server reported no name", t.name,
                     server_name);
        continue;
      }
      spdlog::debug("Registered MCP tool: mcp.{}.{}", server_name, t.name);
    }
  }
}

void Runtime::RebuildToolbox(const config::AgentEntry& agent) {
  mcp::DisconnectMcpServers(mcp_clients_);
  mcp_clients_ = mcp::ConnectMcpServers(agent.mcp_servers);

  toolbox_.Clear();
  RegisterBuiltinTools(agent);
  RegisterMcpTools();

  auto security = agent.security;
  security.sandbox_root = ResolveWorkspacePath(workspace_root_, security.sandbox_root).string();
  executor_->SetSecurityPolicy(std::move(security));
  executor_->SetSystemPrompt(agent.backend.system_prompt.value_or(""));
  agent_manager_->SetActiveAgent(agent.name);
}

void Runtime::Activate(const config::AgentEntry& agent) {
  RebuildToolbox(agent);
  if (!current_session_) return;
  try {
    current_session_->SetAgent(agent.name);
  } catch (const std::exception& e) {
    spdlog::warn("Failed to sync session config: {}", e.what());
    return;
  }
  SaveCurrentSession();
}

void Runtime::SwitchAgent(const config::AgentEntry& new_agent) {
  if (agent_manager_->GetActiveAgent() == new_agent.name) return;
  Activate(new_agent);
}

void Runtime::SwitchBackend(const config::BackendConfig& backend) {
  config::AgentEntry transient = ActiveAgent();
  transient.name = config::BackendTypeName(backend.type);
  transient.backend = backend;
  transient.description.clear();

  agent_manager_->Adopt(transient);
  Activate(transient);
}

}  // namespace pu
