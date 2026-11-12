// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cassert>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pu/config/agent_manager.hpp"
#include "pu/config/agents.hpp"
#include "pu/core/base.hpp"
#include "pu/core/http_client.hpp"
#include "pu/runtime/executor.hpp"
#include "pu/mcp/client.hpp"
#include "pu/runtime/command_router.hpp"
#include "pu/tools/delegation.hpp"
#include "pu/paging/paging.hpp"
#include "pu/session/session.hpp"
#include "pu/tools/toolbox.hpp"

namespace pu {

class Runtime {
 public:
  Runtime() = default;
  ~Runtime() = default;

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  // The directory whose `.pu/` this runtime reads and writes: the agents table, the session
  // and the sandbox all resolve from here. Set before Initialize, because Initialize is where
  // the three are read. Defaults to the current directory, so a caller that never calls this
  // gets the behaviour of a plain `cd`-then-run.
  void SetWorkspaceRoot(std::filesystem::path root) { workspace_root_ = std::move(root); }

  // A turn with no memory: Initialize does not load `.pu/session.json` and every save is a
  // no-op. For a caller that supplies the whole context and wants only an answer — the
  // delegated turn, and `pu ask`. A remembered conversation is what `chat` and `serve` are
  // for; two ways to hold one would disagree about what a second run of the same prompt means.
  void SetEphemeral(bool ephemeral = true) { ephemeral_ = ephemeral; }

  void Initialize(const std::string& config_path = "");

  DelegationResult Delegate(const Delegation& request);
  void Shutdown();

  ExecutionResult ProcessInput(
      const std::string& input, CancelToken cancel_token = nullptr,
      TokenCallback content_callback = nullptr,
      ToolCallbacks tool_callbacks = {},
      TokenCallback reasoning_callback = nullptr,
      CancelToken command_stop_token = nullptr);

  // Whether this turn should stop now: the caller's token, the shutdown below, and the clock.
  // The runtime owns the two things a caller cannot see, which is why it is the runtime that
  // folds them into one answer rather than each round asking three times.
  bool StopRequested() const;

  void SetDefaultAgent(const std::string& agent_name);
  void SwitchAgent(const config::AgentEntry& new_agent);
  void SwitchBackend(const model::Backend& backend);

  std::filesystem::path WorkspaceRoot() const { return workspace_root_; }
  // The file Initialize() loaded.
  const std::string& ConfigPath() const { return config_path_; }

  Toolbox& Tools() {
    assert(is_initialized_ && "Initialize() must run before any toolbox query");
    return toolbox_;
  }

  // Words typed while a turn runs. The next tool boundary carries them to the
  // model; a command killed through the token names them in its result.
  void SetUserNote(std::string note) {
    assert(is_initialized_ && "Initialize() must run before any note");
    executor_->SetUserNote(std::move(note));
  }

  // The approval gate. Ask fires on the turn thread when a command matches the
  // policy's approval patterns; Resolve answers it from the serve thread.
  void EnableApprovalGate() {
    assert(is_initialized_ && "Initialize() must run before any gateway");
    executor_->EnableApprovalGate();
  }
  void SetOnApprovalAsk(std::function<void(const std::string&)> cb) {
    executor_->SetOnApprovalAsk(std::move(cb));
  }
  void ResolveApproval(bool allow, std::string note) {
    executor_->ResolveApproval(allow, std::move(note));
  }

  AgentManager& Agents() {
    assert(agent_manager_ && "Initialize() must run before any agent query");
    return *agent_manager_;
  }
  // Reading who is configured does not change anything, so it is answered from a const
  // Runtime as well: the summary a surface prints is a read.
  const AgentManager& Agents() const {
    assert(agent_manager_ && "Initialize() must run before any agent query");
    return *agent_manager_;
  }
  pu::http::HttpClient& Http() {
    assert(is_initialized_ && "Initialize() must run before any HTTP call");
    return *http_client_;
  }
  // For a collaborator that outlives the call and needs the client shared, which a
  // provider built per model does.
  std::shared_ptr<pu::http::HttpClient> SharedHttp() {
    assert(is_initialized_ && "Initialize() must run before any HTTP call");
    return http_client_;
  }
  std::shared_ptr<Session> DefaultSession();

  const config::AgentEntry& ActiveAgent() const;

  bool RewindBefore(size_t turn);

  model::Backend CurrentBackend() const;

  ThinkingLevel CurrentThinkingLevel() const;
  // How much context the active model holds, from the file's own `context_window`, or `0` when
  // it names none. Nothing is read over the network, and the reason is the lock rather than the
  // client: this is asked while the session's lock is held, so a catalogue read here would stand
  // between the page and every other request for as long as the gateway takes — thirty seconds
  // at the client's own budget, and a turn's own turn waits behind it. `pu check --caps` prints
  // what a catalogue says, which is where the number to write comes from.
  //
  // Advisory: no prompt is trimmed against this and no turn is refused for outgrowing it — a
  // wrong number draws a misleading bar, never a broken turn.
  int ContextWindow() const;
  std::optional<ThinkingLevel> ThinkingOverride() const;
  void SetThinkingLevel(std::optional<ThinkingLevel> level);

  // What this session overrides, and what is in force once the overrides are applied to the
  // agent's own configuration. `Settings()` renders both the value and where it came from.
  // The overrides are copied because a request thread sets them while a turn reads them.
  session::Overrides Overrides() const;
  std::vector<session::SettingView> Settings() const;
  // One setting from a request: `null` clears it, anything else is checked against its row.
  void SetSetting(const std::string& name, const boost::json::value& raw);
  void ClearSettings();

 private:
  model::Backend ConfiguredBackend() const;
  // The budget the agent's configuration names, which is what a cleared override returns to.
  int ConfiguredToolBudget() const;
  // Pushes the budget in force into the executor, after an override changed or an agent switch.
  void ApplyToolBudget();
  void Activate(const config::AgentEntry& agent);
  void RebuildToolbox(const config::AgentEntry& agent);
  void SaveCurrentSession();
  void RegisterBuiltinTools(const config::AgentEntry& agent);
  void RegisterMcpTools();

  bool is_initialized_ = false;
  std::unique_ptr<AgentManager> agent_manager_;
  std::unique_ptr<CommandRouter> command_router_;
  // Shared so the TLS context, built from the OS certificate store, outlives a single turn.
  std::shared_ptr<pu::http::HttpClient> http_client_;
  Toolbox toolbox_;
  std::unique_ptr<Executor> executor_;
  // One store per runtime, beside the toolbox rather than inside it: a model switch
  // rebuilds the tools, and a page the model was told it could read again has to stay
  // readable across that.
  paging::Pager pager_;
  std::filesystem::path workspace_root_;
  // Whether this runtime is itself answering a delegated turn. A delegated runtime registers no
  // `ask_dir`, so a chain stops here rather than at a depth counter somebody has to carry.
  bool delegated_ = false;
  bool ephemeral_ = false;
  // The stop the current turn answers to, when it is a delegated turn: the asking turn's token
  // and the clock, folded into one by StopRequested(). Both are empty for an ordinary turn, whose
  // caller passes its token to ProcessInput directly.
  CancelToken turn_cancel_;
  std::chrono::steady_clock::time_point turn_deadline_{};  // zero: no deadline
  std::shared_ptr<Session> current_session_;
  // Guards the session's overrides: a serve request sets them on one thread while a turn reads
  // them on another.
  mutable std::mutex overrides_mutex_;


  std::string default_agent_override_;
  std::string config_path_;

  std::vector<std::shared_ptr<mcp::McpClient>> mcp_clients_;
};

}  // namespace pu
