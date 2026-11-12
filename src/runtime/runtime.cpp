// SPDX-License-Identifier: GPL-3.0-only
#include "pu/runtime/runtime.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <system_error>
#include <variant>

#include <boost/json.hpp>
#include <spdlog/spdlog.h>
#include "pu/core/json.hpp"

#include "pu/config/agents.hpp"
#include "pu/tools/ask_dir_tool.hpp"
#include "pu/core/logging.hpp"
#include "pu/core/text.hpp"
#include "pu/core/base.hpp"
#include "pu/core/beast_http_client.hpp"
#include "pu/core/platform.hpp"
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
  const auto default_entry = std::ranges::find_if(
      agents_cfg.agents,
      [&](const config::AgentEntry& entry) { return entry.name == active_agent; });
  if (default_entry == agents_cfg.agents.end()) {
    const std::string known = text::Join(
        std::views::transform(agents_cfg.agents,
                              [](const config::AgentEntry& entry) { return entry.name; }),
        " ");
    throw GenericError("Requested agent is not configured: " + active_agent +
                       (known.empty() ? " (no agents are configured)" : "; configured: " + known));
  }

  agent_manager_ = std::make_unique<AgentManager>();
  agent_manager_->SetActiveAgent(active_agent);
  agent_manager_->LoadAgentConfigs(agents_cfg.agents);

  command_router_ = std::make_unique<CommandRouter>(*this);

  executor_ = std::make_unique<Executor>();
  // The executor is the only place a turn spends its time, so it is the place that has to hear
  // about a deadline: the token it already receives covers a caller that cancels, and this covers
  // the two stops that have no token. A runtime that is never delegated leaves both unset, and
  // then the check answers `ShutdownRequested()` alone, which is what it answered before.
  executor_->SetStopCheck([this] { return StopRequested(); });
  http_client_ = std::make_shared<pu::http::BeastHttpClient>();

  RebuildToolbox(*default_entry);

  // An ephemeral runtime wakes up with no past: a stored conversation belongs to the world
  // that asked for it, and this one was asked to answer a question, not to continue one.
  const auto session_path = workspace_root_ / ".pu" / "session.json";
  if (!ephemeral_ && std::filesystem::exists(session_path)) {
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
  if (!current_session_ || ephemeral_) return;
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

ExecutionResult Runtime::ProcessInput(const std::string& input, CancelToken cancel_token,
                                      TokenCallback content_callback,
                                      ToolCallbacks tool_callbacks,
                                      TokenCallback reasoning_callback,
                                      CancelToken command_stop_token) {
  ExecutionResult result;
  try {
    BeginRequest();

    // The tools were built before this turn existed, so the stop this turn was given has to be
    // handed to them now: a page cancels a turn rather than a runtime, and a token captured at
    // build time would be the previous turn's. Delegation is the one tool that outlives its own
    // turn while it waits, which is why it is the one that needs the current token.
    for (Tool* tool : toolbox_.Tools()) {
      auto* asking = dynamic_cast<tools::AskDirTool*>(tool);
      if (asking != nullptr) asking->SetCancelToken(cancel_token);
    }

    auto session = DefaultSession();

    if (!input.empty() && input[0] == '/') {
      const CommandReply reply = command_router_->Route(input, *session);
      result.content = reply.text;
      result.was_command = true;
      result.was_streamed = false;
      result.has_error = !reply.ok;
      if (!reply.ok) result.error_message = reply.text;
      SaveCurrentSession();
      return result;
    }

    const config::Backend backend = CurrentBackend();
    auto provider = session->CreateProvider(backend, http_client_);
    auto exec_result =
        executor_->Execute(input, session->Messages(),
                           {.provider = provider.get(),
                            .toolbox = &toolbox_,
                            .content_callback = content_callback,
                            .tool_callbacks = tool_callbacks,
                            .reasoning_callback = reasoning_callback,
                            .cancel_token = cancel_token,
                            .command_stop_token = command_stop_token});
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

DelegationResult Runtime::Delegate(const Delegation& request) {
  DelegationResult result;

  const std::filesystem::path target =
      request.dir.empty() ? workspace_root_ : request.dir;
  const std::filesystem::path table = target / ".pu" / "agents.json";
  const std::filesystem::path inherited = target / ".pu" / "agents";
  std::error_code ec;
  if (!std::filesystem::exists(table, ec) && !std::filesystem::is_directory(inherited, ec)) {
    // The remedy rides with the refusal. A model that is told only what is missing asks the
    // same question again, because "no table here" names no action it can take; the file it
    // would have to write is the action, and it is one line of a shape it already knows.
    result.error =
        "'" + target.string() +
        "' configures no agent: it has no .pu/agents.json. Write one with write_file, in the "
        "shape of this workspace's own .pu/agents.json ({\"default_agent\": \"<name>\", "
        "\"agents\": [{\"name\": \"<name>\", \"backend\": {\"gateway\": ..., \"model\": ...}}]}), "
        "then ask again.";
    return result;
  }

  try {
    Runtime child;
    child.workspace_root_ = target;
    // Ephemeral on both sides: the other directory's stored conversation is neither read nor
    // written, so the same question asked twice costs the same and means the same.
    child.SetEphemeral();
    if (!request.agent.empty()) child.SetDefaultAgent(request.agent);
    child.delegated_ = true;
    // The asking turn's stop reaches the child as its own, so the stop lands in the turn that is
    // waiting rather than in the one that has already handed the question over.
    child.turn_cancel_ = request.cancel_token;
    if (request.timeout.count() > 0) {
      child.turn_deadline_ = std::chrono::steady_clock::now() + request.timeout;
    }
    child.Initialize();

    const ExecutionResult answered =
        child.ProcessInput(request.prompt, request.cancel_token);
    if (const auto session = child.DefaultSession()) result.usage = session->Usage().Total();
    child.Shutdown();

    // Read before the error branches: a stopped turn ends with no reply and no error, and the
    // two refusals below would otherwise describe it as an empty answer.
    result.stopped = request.cancel_token && request.cancel_token->load(std::memory_order_acquire);
    if (result.stopped) {
      result.error = "'" + target.string() +
                     "' was still answering when the turn was stopped, so its answer was "
                     "dropped. Ask again if the answer is still wanted";
      return result;
    }
    if (child.turn_deadline_ != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() >= child.turn_deadline_) {
      // Named as a ceiling rather than as a failure: the turn did nothing wrong, it ran out of
      // the time it was given, and the number is what a caller changes.
      result.error = "'" + target.string() + "' did not answer within " +
                     std::to_string(request.timeout.count()) +
                     "ms. Ask something smaller, or give it longer";
      return result;
    }

    if (answered.has_error) {
      result.error = answered.error_message.empty() ? answered.content : answered.error_message;
      return result;
    }
    if (answered.was_command) {
      // A line the asking agent meant as a question, read as a command, is not an answer: the
      // text it produced is a page of help or a setting, and handing it back as a conclusion
      // would report something nobody asked for. Nothing was asked of a provider either, so
      // this is a mistake in the call rather than a turn that went wrong. The prompt is
      // therefore named as the thing to change, since it is the only input this call controls.
      result.error = "the question began with '/' and was read as a pu command, not a question: "
                     "'" + request.prompt +
                     "'. Reword it so it does not start with a slash";
      return result;
    }
    if (answered.content.empty()) {
      // Not retried with the same words: an empty turn is a fact about the question or the
      // provider, and the honest next move is to say more, not to say it again.
      result.error = "'" + target.string() +
                     "' answered with nothing. Ask again in more specific terms, or ask "
                     "something smaller";
      return result;
    }
    result.answer = answered.content;
    return result;
  } catch (const std::exception& e) {
    result.error = std::string("could not ask '") + target.string() + "': " + e.what();
    return result;
  }
}

void Runtime::SetDefaultAgent(const std::string& agent_name) {
  default_agent_override_ = agent_name;
}

bool Runtime::StopRequested() const {
  // A delegated turn's stop is three things folded into one question. The caller's token is the
  // one a person pressed a button for; the deadline is the one nobody pressed anything for; and a
  // shutting-down process is a stop that was never asked for and cannot be reported to anybody,
  // so it is answered here rather than surfacing as a failed turn in a log nobody reads.
  if (turn_cancel_ && turn_cancel_->load(std::memory_order_acquire)) return true;
  if (turn_deadline_ != std::chrono::steady_clock::time_point{} &&
      std::chrono::steady_clock::now() >= turn_deadline_) {
    return true;
  }
  return platform::ShutdownRequested();
}

void Runtime::RegisterBuiltinTools(const config::AgentEntry& agent) {
  // The budget is the store's, and the tool reads it from there: one owner for the page
  // size and the retention, applied before the tool that will obey it is built.
  pager_.SetBudget(agent.paging);

  // Which builtins this agent has. An empty list means every one of them, which is what a
  // table written before the field existed meant and must keep meaning. A tool named here and
  // not registered is the entry's mistake and `pu check` refuses it, so this reads the list
  // rather than validating it a second time.
  const auto has = [&](const char* name) {
    if (agent.tools.empty()) return true;
    return std::find(agent.tools.begin(), agent.tools.end(), name) != agent.tools.end();
  };

  if (has("bash")) {
    toolbox_.RegisterTool(std::make_unique<tools::BashTool>(
        ResolveWorkspacePath(workspace_root_, agent.security.sandbox_root).string(), pager_));
  }
  if (has("write_file")) toolbox_.RegisterTool(std::make_unique<tools::WriteFileTool>());
  // A runtime that is itself answering a delegated turn gets no way to ask one further: the
  // chain that could grow is the one a tool starts, and this is where it does not start.
  if (has("ask_dir") && !delegated_) {
    // The turn's own stop is handed over per turn (see ProcessInput), since the toolbox outlives
    // the turn that built it; a runtime with no turn in flight hands over nothing, and the
    // delegate's deadline is what stops it then.
    // The pager is this runtime's own, so an answer's rank continues the transcript's and a
    // second answer to one question elides the first. Without it the answer went in as prose
    // with no origin, and prose is the one thing the projection never compresses.
    toolbox_.RegisterTool(std::make_unique<tools::AskDirTool>(workspace_root_, agent.delegates,
                                                              &pager_, turn_cancel_));
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
