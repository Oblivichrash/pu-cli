// SPDX-License-Identifier: GPL-3.0-only
#include "pu/runtime/runtime.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>
#include <variant>

#include <boost/json.hpp>
#include <spdlog/spdlog.h>
#include "pu/core/json.hpp"

#include "pu/config/agents.hpp"
#include "pu/config/peer.hpp"
#include "pu/tools/peer_tool.hpp"
#include "pu/core/logging.hpp"
#include "pu/core/base.hpp"
#include "pu/core/beast_http_client.hpp"
#include "pu/session/session.hpp"
#include "pu/tools/bash.hpp"
#include "pu/tools/write_file.hpp"
#include "pu/tools/mcp_tool.hpp"

namespace pu {
namespace {

std::filesystem::path ResolveWorkspacePath(const std::filesystem::path& root,
                                           const std::string& configured_path) {
  const auto path =
      configured_path.empty() ? std::filesystem::path(".") : std::filesystem::path(configured_path);
  return path.is_absolute() ? path : root / path;
}

// A session this build cannot read is moved aside rather than left to be overwritten by
// the first save of the fresh conversation that replaces it. The backup is one file, so a
// second unreadable session replaces it: a backup left unread is one nobody needed.
void SetAside(const std::filesystem::path& path, const std::string& reason) {
  std::filesystem::path backup = path;
  backup += ".bak";

  std::error_code ec;
  std::filesystem::rename(path, backup, ec);

  spdlog::error(
      "session.json cannot be loaded by this build, so a fresh conversation starts.\n"
      "  file:    {}\n"
      "  reason:  {}\n"
      "  kept at: {}",
      path.string(), reason, ec ? "(left in place: " + ec.message() + ")" : backup.string());
}

std::shared_ptr<Session> LoadSessionFromFile(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) return nullptr;

  boost::json::value j;
  {
    std::ifstream file(path);
    if (!file.is_open()) return nullptr;

    try {
      file >> j;
    } catch (const std::exception& e) {
      // The stream is closed by scope exit: Windows refuses to rename a file with an
      // open handle, so the set-aside below would silently do nothing.
      SetAside(path, std::string("body does not parse: ") + e.what());
      return nullptr;
    }
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
  SetAside(path, reason);
  return nullptr;
}

// The highest rank the transcript already carries. A pager opening that transcript has to
// continue from it: ranks only compare against each other, so a fresh one starting at 1 would
// rank every run made after a restart below every page read before it, and elision would never
// fire again.
std::size_t HighestRankIn(const Session& session) {
  std::size_t highest = 0;
  for (const session::MessageNode* node : session.Messages().Chain()) {
    const auto* receipt = std::get_if<session::ToolPayload>(&node->payload);
    if (receipt == nullptr || !receipt->origin) continue;
    highest = std::max(highest, receipt->origin->rank);
  }
  return highest;
}

}  // namespace

void Runtime::Initialize(const std::string& config_path) {
  if (is_initialized_) return;

  if (workspace_root_.empty()) workspace_root_ = std::filesystem::current_path();

  std::string log_level = std::getenv("PU_LOG_LEVEL") ? std::getenv("PU_LOG_LEVEL") : "";
  pu::InitLogging(log_level);

  std::string cfg_path =
      config_path.empty() ? (workspace_root_ / ".pu" / "agents.json").string() : config_path;

  const char* explicit_config = std::getenv("PU_AGENTS_CONFIG");
  if (explicit_config && *explicit_config) {
    if (!std::filesystem::exists(explicit_config)) {
      throw GenericError(std::string("PU_AGENTS_CONFIG points at a missing file: ") + explicit_config);
    }
    cfg_path = explicit_config;
  } else if (!std::filesystem::exists(cfg_path)) {
    cfg_path = config::FindConfigPath();
  }
  if (cfg_path.empty()) {
    throw GenericError("Configuration file not found. Place agents.json in ./.pu/ or ~/.pu/.");
  }

  config_path_ = cfg_path;

  auto agents_cfg = config::LoadAgentsConfig(cfg_path);

  const std::string active_agent =
      default_agent_override_.empty() ? agents_cfg.default_agent : default_agent_override_;
  const auto default_entry =
      std::find_if(agents_cfg.agents.begin(), agents_cfg.agents.end(),
                   [&](const config::AgentEntry& entry) { return entry.name == active_agent; });
  if (default_entry == agents_cfg.agents.end()) {
    std::string known;
    for (const auto& entry : agents_cfg.agents) known += " " + entry.name;
    throw GenericError("Requested agent is not configured: " + active_agent +
                (known.empty() ? " (no agents are configured)" : "; configured:" + known));
  }

  agent_manager_ = std::make_unique<AgentManager>();
  agent_manager_->SetActiveAgent(active_agent);
  agent_manager_->LoadAgentConfigs(agents_cfg.agents);

  command_router_ = std::make_unique<CommandRouter>(*this);

  executor_ = std::make_unique<Executor>();
  http_client_ = std::make_shared<pu::http::BeastHttpClient>();

  peer_config_ = config::FindPeerConfig();
  if (peer_config_.has_value()) {
    spdlog::info("Peer '{}' knows {} peer(s)",
                 peer_config_->self_name.empty() ? "<unnamed>" : peer_config_->self_name,
                 peer_config_->peers.size());
  }

  RebuildToolbox(*default_entry);

  const auto session_path = workspace_root_ / ".pu" / "session.json";
  if (std::filesystem::exists(session_path)) {
    current_session_ = LoadSessionFromFile(session_path);
    // A page read before the restart keeps its place in the order, so the runs made after it
    // arrive behind it rather than at the front.
    if (current_session_ != nullptr) {
      pager_.ContinueRankingFrom(HighestRankIn(*current_session_));
    }
  }

  if (current_session_) {
    auto& spec = current_session_->Spec();
    const bool agent_chosen_here = !default_agent_override_.empty();
    const std::string remembered = agent_chosen_here ? active_agent : spec.agent_name;
    const auto* stored = agent_manager_->AgentConfig(remembered);
    if (stored == nullptr) {
      spdlog::warn("Session names an agent that is not configured: {}", remembered);
      spec.agent_name = active_agent;
    } else {
      spec.agent_name = stored->name;
      if (stored->name != active_agent) {
        RebuildToolbox(*stored);
      }
    }
  }

  is_initialized_ = true;
}

void Runtime::Shutdown() {
  SaveCurrentSession();
  mcp::DisconnectMcpServers(mcp_clients_);
  // Async logging only writes on its own schedule, so a process that ends abruptly would leave
  // the last lines, which are the ones that explain the ending, unwritten.
  FlushLogging();
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
  if (!DefaultSession()->Messages().RewindBefore(turn)) return false;
  SaveCurrentSession();
  return true;
}

std::shared_ptr<Session> Runtime::DefaultSession() {
  if (current_session_) return current_session_;

  auto session = std::make_shared<Session>();
  session->SetAgent(agent_manager_->ActiveAgentName());

  current_session_ = session;
  return current_session_;
}

const config::AgentEntry& Runtime::ActiveAgent() const {
  return *agent_manager_->AgentConfig(agent_manager_->ActiveAgentName());
}

std::string Runtime::HelpText() {
  assert(command_router_ && "Initialize() must run before any help query");
  return command_router_->Help();
}

config::Backend Runtime::ConfiguredBackend() const {
  if (current_session_) {
    const auto& spec = current_session_->Spec();
    if (const auto* configured = agent_manager_->AgentConfig(spec.agent_name)) {
      return configured->backend;
    }
  }

  return ActiveAgent().backend;
}

config::Backend Runtime::CurrentBackend() const {
  const config::Backend backend = ConfiguredBackend();
  if (!current_session_) return backend;
  return session::InForce::Resolve(backend, ConfiguredToolBudget(), Overrides()).backend;
}

ThinkingLevel Runtime::CurrentThinkingLevel() const { return CurrentBackend().thinking; }

int Runtime::ContextWindow() const {
  // The file's figure, and nothing else. A catalogue read here would be a network call under the
  // session's lock, and this build's HTTP client has no deadline to end it.
  return ConfiguredBackend().context_window.value_or(0);
}

std::optional<ThinkingLevel> Runtime::ThinkingOverride() const {
  if (!current_session_) return std::nullopt;
  std::lock_guard<std::mutex> lock(overrides_mutex_);
  return current_session_->Spec().ThinkingOverride();
}

void Runtime::SetThinkingLevel(std::optional<ThinkingLevel> level) {
  SetSetting("thinking", level ? boost::json::value(ThinkingLevelName(*level))
                               : boost::json::value());
}

session::Overrides Runtime::Overrides() const {
  if (!current_session_) return session::Overrides{};
  std::lock_guard<std::mutex> lock(overrides_mutex_);
  return current_session_->Spec().overrides;
}

std::vector<session::SettingView> Runtime::Settings() const {
  return session::ViewSettings(ConfiguredBackend(), ConfiguredToolBudget(), Overrides());
}

int Runtime::ConfiguredToolBudget() const {
  return ActiveAgent().tool_call_budget.value_or(Executor::kDefaultToolCallBudget);
}

void Runtime::SetSetting(const std::string& name, const boost::json::value& raw) {
  // Checked before it is stored, so a value nothing can read is never written to a session.
  const boost::json::value value = session::ParseSetting(name, raw);
  {
    std::lock_guard<std::mutex> lock(overrides_mutex_);
    auto& overrides = DefaultSession()->Spec().overrides;
    if (value.is_null()) {
      overrides.erase(name);
    } else {
      overrides[name] = value;
    }
  }
  ApplyToolBudget();
  SaveCurrentSession();
}

void Runtime::ClearSettings() {
  {
    std::lock_guard<std::mutex> lock(overrides_mutex_);
    DefaultSession()->Spec().overrides.clear();
  }
  ApplyToolBudget();
  SaveCurrentSession();
}

void Runtime::ApplyToolBudget() {
  if (!executor_) return;
  const session::InForce in_force =
      session::InForce::Resolve(ConfiguredBackend(), ConfiguredToolBudget(), Overrides());
  executor_->SetToolCallBudget(in_force.tool_budget);
}

namespace {

// What this turn cost, filed against the node it produced.
//
// A turn that ran tools leaves several assistant nodes; the reply a reader sees is the last
// one, so that is the node the cost is filed under, and `rounds` says how many provider calls
// stand behind it. A turn that produced no assistant node — an error, a command — spent
// nothing worth a line.
void RecordTurnUsage(Session& session, const ExecutionResult& result,
                     const config::Backend& backend) {
  if (!result.usage) return;
  std::string last_assistant;
  for (const session::MessageNode* node : session.Messages().Chain()) {
    if (std::holds_alternative<session::AssistantPayload>(node->payload)) last_assistant = node->id;
  }
  if (last_assistant.empty()) return;

  session::TurnUsage usage;
  usage.node_id = last_assistant;
  usage.rounds = result.rounds;
  usage.prompt_tokens = result.usage->prompt_tokens;
  usage.completion_tokens = result.usage->completion_tokens;
  usage.cached_tokens = result.usage->cached_tokens;
  usage.cost = result.usage->cost;
  usage.model = result.model;
  usage.gateway = config::GatewayName(backend.gateway);
  session.Usage().Record(usage);
}

}  // namespace

ExecutionResult Runtime::ProcessInput(const std::string& input, bool& is_command,
                                      CancelToken cancel_token,
                                      TokenCallback content_callback,
                                      ToolCallbacks tool_callbacks,
                                      TokenCallback reasoning_callback,
                                      CancelToken command_stop_token,
                                      const std::string& peer_origin) {
  ExecutionResult result;
  try {
    BeginRequest();
    executor_->SetPeerOrigin(peer_origin);

    auto session = DefaultSession();

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

    const config::Backend backend = CurrentBackend();
    auto provider = session->CreateProvider(backend, http_client_);
    auto exec_result =
        executor_->Execute(input, session->Messages(), provider.get(), toolbox_,
                           cancel_token, content_callback, tool_callbacks, reasoning_callback,
                           command_stop_token);
    result = std::move(exec_result);
    RecordTurnUsage(*session, result, backend);
    SaveCurrentSession();
    return result;
  } catch (const std::exception& e) {
    result.has_error = true;
    result.error_message = e.what();
    return result;
  }
}

void Runtime::SetDefaultAgent(const std::string& agent_name) {
  default_agent_override_ = agent_name;
}

void Runtime::RegisterBuiltinTools(const config::AgentEntry& agent) {
  // The budget is the store's, and the tool reads it from there: one owner for the page
  // size and the retention, applied before the tool that will obey it is built.
  pager_.SetBudget(agent.paging);
  toolbox_.RegisterTool(std::make_unique<tools::BashTool>(
      ResolveWorkspacePath(workspace_root_, agent.security.sandbox_root).string(), pager_));
  toolbox_.RegisterTool(std::make_unique<tools::WriteFileTool>());
  if (peer_config_.has_value() && !peer_config_->peers.empty()) {
    toolbox_.RegisterTool(std::make_unique<tools::PeerTool>(*peer_config_, http_client_.get()));
  }
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
  ApplyToolBudget();
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
  if (agent_manager_->ActiveAgentName() == new_agent.name) return;
  Activate(new_agent);
}

void Runtime::SwitchBackend(const config::Backend& backend) {
  config::AgentEntry transient = ActiveAgent();
  transient.name = config::GatewayName(backend.gateway);
  transient.backend = backend;
  transient.description.clear();

  agent_manager_->Adopt(transient);
  Activate(transient);
}

}  // namespace pu
