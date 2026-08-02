# pu-cli Architecture

> "朴散则为器"——《老子》

## Overview

pu-cli is built around five principles:

1. **Session isolation** — Each session owns its workspace, history, and branch tree.
2. **Git-style branching** — Fork/merge isolated workspaces with squash or full history.
3. **Dynamic backend switching** — Switch LLM providers without losing state.
4. **Stateless execution** — `Executor` holds no state; all state lives in `Workspace`/`Session`.
5. **Explicit composition** — `Runtime` is a plain object instantiated by `main()` and injected with its collaborators; there is no global singleton.

---

## Core Components

| Component | Responsibility |
|-----------|----------------|
| `Runtime` | Plain object (no singleton) created by `main()`; owns `AgentManager`, `SessionStore`, `Toolbox`, `Executor`, `CommandRouter`; routes input, manages sessions, rebuilds the tool registry on agent switch |
| `Session` | Aggregate root: `Workspace` + `RuntimeSpec` + `CallStack` |
| `Workspace` | State container: `Transcript` (history) + `Memory` (variables/artifacts) + branch tree (via parent/children links) |
| `Executor` | Fully stateless tool loop; reads/writes `Workspace`, holds no counters or flags |
| `LLMProvider` | Model gateway; handles transport + format adaptation |
| `Toolbox` | Tool registry; rebuilt per active agent, executes built-in and MCP tools |
| `CommandRouter` | Routes `/` commands to handlers |
| `SessionStore` | Persists sessions to `~/.pu/sessions/` |
| `ForkMergeService` | Branch operations: fork, merge, prune; **does not depend on `CallStack`** |
| `CallStack` | Delegation stack for nested tasks; maintained by the upper layer (router/tools) |
| `McpClient` | High-level MCP client: handshake, `ListTools`, `CallTool` |
| `JsonRpcClient` | JSON-RPC 2.0 protocol layer; request/response with promise mapping |
| `StdioTransport` | stdio subprocess transport; fork+exec, pipe-based line I/O |
| `ArtifactExtractor` | Extracts `Artifact`s (file paths, error messages) from workspace history |

---

## Error Handling

All non-recoverable runtime errors derive from a single base class:

```
pu::RuntimeError : std::runtime_error
  ├── pu::Error            (e.g. configuration parsing)
  │     ├── pu::HttpError  (HttpClient failures)
  │     └── pu::StoreError (SessionStore persistence failures)
```

`main()` wraps top-level dispatch in a `try/catch (const std::exception&)` so any
`RuntimeError` (or plain `std::exception`) is converted to a friendly fatal-error
message instead of crashing. Components throw the most specific subclass available;
new error types should extend `RuntimeError`.

---

## Key Data Types

| Old | New |
|-----|-----|
| `Context` | `Workspace` |
| `Delegation` | `Assignment` |
| `Fact` | `Artifact` |
| `SummaryReport` | `HandoffReceipt` |
| `DelegationStack` | `CallStack` |
| `session::BackendConfig` | `SessionBackendConfig` |
| `FactExtractor` | `ArtifactExtractor` |

Serialization follows the *new* names (`artifacts`, `workspace_id`, …). The
deserializer keeps a legacy fallback (`facts`) so pre-rename session files still load.

---

## Runtime Lifecycle & Dependency Injection

`Runtime` is not a singleton. `main()` constructs a single instance and owns every
major collaborator as a `unique_ptr` member; nothing is reached through a global:

```
main()
 ├─ pu::Runtime runtime;            // constructed on the stack
 ├─ RunAsk / RunChat(runtime)
 └─ catch (std::exception&) → friendly message
```

Key responsibilities:

- `Initialize(config_path)` — loads `agents.json`, creates `AgentManager`,
  `SessionStore`, `Executor`, `CommandRouter`, then builds the default toolbox.
- `CreateSession(owner, agent, backend)` — creates a session; `backend` is
  optional and overrides the agent's default backend for that session only.
- `SwitchAgent(agent)` — updates the active agent **and rebuilds the toolbox**
  (see below).
- `ProcessInput(...)` — routes either to `CommandRouter` (commands) or to
  `Executor` (messages).

### Toolbox & MCP lifecycle

The `Toolbox` is **not** a global singleton either. It is rebuilt whenever the
active agent changes:

```
RebuildToolbox(agent)
 ├─ ShutdownMCP()                      // stop previous MCP child process
 ├─ toolbox_ = new Toolbox()
 ├─ RegisterBuiltinTools()
 ├─ if agent.mcp_servers non-empty:
 │    StartMCP(cfg) → ListTools() → register mcp.<name> tools
 └─ executor_->SetToolbox(toolbox_);
     executor_->SetSecurityPolicy(agent.security)
     executor_->SetCompactionConfig(agent.compaction)
```

Consequences:

- Switching agents via `/backend <agent>` tears down the old MCP server and
  starts the new agent's server on demand — MCP clients are **not** kept alive
  across agent switches.
- Currently only the first `mcp_servers` entry per agent is started.
- Compaction settings travel with the agent: `SetCompactionConfig` copies the
  agent's `HistoryCompactionConfig` (`enabled`, `keep_head`, `keep_tail`) into
  the executor before any message is processed.

---

## Data Flow

```
User Input
    │
    ▼
Runtime.ProcessInput(session_id, input, ...)
    │
    ├── Is command? ──► CommandRouter ──► Session mutation
    │
    └── Is message? ──► Session.CreateProvider()
                         │
                         ▼
                       Executor.Execute()          (stateless)
                         │
                         ├── Read Workspace.Transcript
                         ├── LLMProvider.Chat()
                         ├── Toolbox.ExecuteTool() (per active agent)
                         ├── Write to Workspace.Transcript
                         └── Return response
                         │
                         ▼
                       SessionStore.SaveSession()
```

### Fork/Merge flow (decoupled coordination)

`ForkMergeService` operates purely on `Workspace` nodes. It never touches
`CallStack`; the **upper layer** (either `CommandRouter` for `/fork`/`/merge`
or the fork/merge tools) is responsible for pushing/popping the corresponding
`Assignment`:

```
Fork:  parent = CallStack.CurrentWorkspace() or GetRootWorkspace()
       result = ForkMergeService.Fork(parent, agent, goal)
       CallStack.Push(assignment, result.child_workspace)

Merge: child  = CallStack.CurrentWorkspace()
       result = ForkMergeService.Merge(child, message, strategy)
       if succeeded → CallStack.Pop()
```

This keeps the branch service independent from the delegation stack and lets
both the CLI commands and the tool interface share the same coordination rules.

---

## MCP Integration

```
┌──────────────────────────────────────────────┐
│            pu::mcp::McpClient                │  ← High-level: Initialize/ListTools/CallTool
└──────────────────────┬───────────────────────┘
                       │
┌──────────────────────▼───────────────────────┐
│          pu::mcp::JsonRpcClient              │  ← JSON-RPC 2.0: request/response, promise map
└──────────────────────┬───────────────────────┘
                       │
┌──────────────────────▼───────────────────────┐
│          pu::mcp::StdioTransport             │  ← Child process stdio, line-delimited JSON
└──────────────────────────────────────────────┘
```

MCP servers are configured per-agent in `agents.json` via `mcp_servers`. When an
agent becomes active, `Runtime::RebuildToolbox` spawns its server, performs the
MCP handshake, lists tools, and registers them with a `mcp.` prefix in the
current `Toolbox`. Switching agents shuts the previous server down. No external
MCP SDK is required — the client is fully self-contained.

---

## Transcript Compaction

`Transcript::Compact(keep_head, keep_tail)` keeps the head and tail messages and
discards the middle. The counts come from the agent's `history_compaction`
config (defaults `keep_head = 10`, `keep_tail = 50`); there are no hardcoded
limits in the transcript. The non-obvious part is **tool-call pairing
preservation**:

1. Compute the tail start as `size - keep_tail`.
2. Scan **backward** from the tail start to `keep_head`.
3. For every assistant message carrying `tool_calls_json`, collect the tool-call
   IDs and check that **all** of them have a matching `role == "tool"` response
   later in the transcript.
4. If any ID is missing its response, extend `tail_start` leftward to include
   that assistant message, so a truncated assistant message is never separated
   from the tool responses it needs.

If a tool-call JSON fails to parse, the message is skipped (the pair check
cannot be verified), which is a conservative fallback that may keep slightly
more history than strictly necessary.

### When compaction runs

`Executor::Execute()` decides, once per request, whether to compact:

```
if (!provider->SupportsTools() && !tools.empty() && compaction_config_.enabled)
    if provider->IsThinkingMode() → log warning, skip compaction
    else → workspace.Compact(keep_head, keep_tail)
```

`LLMProvider::IsThinkingMode()` is a virtual hook (default `false`). It is
overridden by `OpenAIProvider` (returns `config_.enable_thinking`, used for
DeepSeek/vLLM reasoning models) and `OllamaProvider` (returns `false`).

Compaction is skipped for thinking-mode backends on purpose: dropping the
intermediate `reasoning_content` messages would break the DeepSeek API contract
(HTTP 400), so the full history is sent instead. `Workspace::Compact` simply
forwards its arguments to `Transcript::Compact`, keeping the delegate interface
thin.

---

## Persistence

```
~/.pu/sessions/
├── session_123456.json   # Full session state
└── session_123457.json
```

Each session file contains serialized `Workspace`, `RuntimeSpec`, and `CallStack`.

---

## Directory Structure

```
include/pu/
├── agent/          # AgentManager, config (config::BackendConfig, BackendType)
├── runtime/        # Runtime, CommandRouter
├── session/        # Session, Workspace, Transcript, Memory, CallStack
├── executor/       # Executor (stateless)
├── llm/            # LLMProvider, providers (Ollama, OpenAI)
├── mcp/            # McpClient, JsonRpcClient, StdioTransport, types
├── tools/          # Toolbox, tools (including McpTool adapter)
├── storage/        # SessionStore
└── core/           # ForkMergeService, SummaryGenerator, ArtifactExtractor

src/
├── app/            # CLI entry (main constructs Runtime), UI helpers
├── runtime/        # Runtime, CommandRouter implementations
├── session/        # Session, Workspace, etc. implementations
├── executor/       # Executor implementation
├── llm/            # Providers
├── mcp/            # MCP transport, JSON-RPC, client implementations
├── tools/          # Toolbox, tools
├── storage/        # SessionStore
└── core/           # ForkMergeService, SummaryGenerator, ArtifactExtractor
```

---

## Extension Points

- **New backend**: Implement `LLMProvider` and register in `Session::CreateProvider()`.
- **New tool**: Inherit `pu::Tool`, implement methods, register in `Runtime::RegisterBuiltinTools()`.
- **New command**: Add handler in `CommandRouter`, route, update help.
- **External tool (no C++)**: Add an `mcp_servers` entry to `agents.json` — tools are discovered automatically when the agent becomes active.

---

## Known Limitations

- `--agent` startup parameter does not auto-switch backend (use `/backend` after start).
- `[INFO] Connected to agent:` display is currently empty.
- MCP transport is POSIX-only (`fork`/`execvp`); Windows support not yet implemented.
- MCP request timeout is fixed at 5 seconds.
- Only the first `mcp_servers` entry per agent is started.
- Compaction is only truncation-based today; `"strategy": "summarize"` is reserved for future work and remains incompatible with thinking mode.

---

## License

GPL-3.0 — see [LICENSE](../LICENSE)
