# pu-cli Architecture

> "朴散则为器"——《老子》

## Overview

pu-cli is built around four principles:

1. **Single-session auto-persistence** — One session owns the workspace and history, transparently persisted.
2. **Dynamic backend switching** — Switch LLM providers without losing state.
3. **Stateless execution** — `Executor` keeps no per-session state; all session state lives in `Workspace`/`Session` (`Executor` holds only configuration and an environment-probe cache).
4. **Explicit composition** — `Runtime` is a plain object instantiated by `main()` and injected with its collaborators; there is no global singleton.

---

## Tech Stack

The runtime dependencies are **Boost** (Beast, Asio, JSON, ProgramOptions),
**spdlog**, and **OpenSSL**:

- **Boost.Beast** — HTTP/WebSocket server (`pu serve`) and HTTP client
  (`BeastHttpClient`), built on top of **Boost.Asio**.
- **Boost.Asio** — asynchronous I/O core underneath Beast.
- **Boost.JSON** — JSON parsing/serialization (config, session persistence,
  WebSocket protocol, MCP, REST API).
- **Boost.ProgramOptions** — CLI option parsing.
- **OpenSSL** — TLS for `https://`/`wss://` connections in `BeastHttpClient`.
- **spdlog** — structured logging.

---

## Core Components

| Component | Responsibility |
|-----------|----------------|
| `Runtime` | Plain object created by `main()`; owns `AgentManager`, `Toolbox`, `Executor`, `CommandRouter`; routes input, holds the single `Session`, rebuilds tool registry on agent switch |
| `Session` | Aggregate root: `Workspace` + `RuntimeSpec` |
| `Workspace` | State container: `Transcript` (history) + `Memory` (variables/artifacts) |
| `Executor` | Session-state-free tool loop (holds config + probe cache); reads/writes `Workspace`; injects system context and processes structured tool output |
| `LLMProvider` | Model gateway; handles transport + format adaptation |
| `Toolbox` | Tool registry; rebuilt per active agent, executes built-in and MCP tools |
| `CommandRouter` | Routes `/` commands to handlers |
| `Web Server` | `pu serve` (`RunServe` in `src/app/serve.cpp`, with REST handlers in `serve_http_routes.cpp` and the WebSocket protocol in `serve_websocket.cpp`): Boost.Beast HTTP/WebSocket server exposing the session via WebSocket (`/ws`) for chat and REST endpoints for control/status |
| `McpClient` | High-level MCP client: handshake, `ListTools`, `CallTool` |
| `JsonRpcClient` | JSON-RPC 2.0 protocol layer |
| `Transport` | Abstract MCP transport (`Start` / `Stop` / `WriteLine`) |
| `StdioTransport` | stdio subprocess transport |
| `HttpTransport` | remote streamable-HTTP transport (BeastHttpClient POST, line-delimited responses) |

---

## Error Handling

All non-recoverable runtime errors derive from a single base class:

```
pu::RuntimeError : std::runtime_error
  ├── pu::Error            (e.g. configuration parsing)
  │     └── pu::HttpError  (HttpClient failures)
```

`main()` wraps top-level dispatch in a `try/catch (const std::exception&)` so any
`RuntimeError` is converted to a friendly fatal-error message.

---

## JSON Handling

All JSON parsing and serialization is provided by **Boost.JSON**
(`boost::json::value`; Boost >= 1.75). `include/pu/core/json.hpp` is a thin
convenience layer over the Boost API for the operations the codebase uses most:

- `pu::json::parse` / `pu::json::serialize` — parse and serialize
  (`boost::json::parse` throws `boost::system::system_error` on malformed input).
- `pu::json::ValueOrDefault(value, key, def)` — optional member read with default.
- `pu::json::HasKey(value, key)` — safe key-existence check.
- `pu::json::PrettyPrint(value)` — indented output for `agents.json` and
  `session.json`.

JSON is used for configuration (`agents.json`), session persistence
(`Session::Serialize` / `Session::Deserialize`), structured tool output
(`pu::tools::tool_result.hpp`), the MCP JSON-RPC layer, and the WebSocket/REST
API in `src/app/serve_http_routes.cpp` and `src/app/serve_websocket.cpp`.

---

## Executor

### Structured Tool Output

Tools return structured JSON instead of free text. `Executor` extracts `stdout`
(on success) or `error` (on failure) and stores only that content in the
transcript, so the full JSON is never persisted and history stays readable. The
field schema lives in `include/pu/tools/tool_result.hpp` and is documented in
[README](../README.md#tool-output-format).

### System Context Injection

`Executor` automatically builds a system message containing:

- OS name and kernel version (probed once at startup)
- Security policy (sandbox root, forbidden patterns)
- Current working directory (the sandbox root)
- Tool-use guidelines for the model

Artifacts are persisted in the session (`Workspace`/`Memory`) but are not
injected into the prompt.

This context is merged with the agent's configured `system_prompt` and prepended
to the stored turns when the request view is rendered. `Runtime::RebuildToolbox`
passes the prompt to `Executor::SetSystemPrompt`, so the prompt comes from the
agent configuration that declares it; it is not session state, and switching the
backend no longer clears it.

### Environment Probing

`Executor::ProbeStaticEnvironment()` runs once during construction and uses
`uname -s` / `uname -r` on POSIX (or the Windows kernel API) to detect the OS
name and kernel version. The result is cached and included in the system
context. No tool-binary detection (`which`) is performed.

### Forbidden Patterns

`forbidden_patterns` is enforced at the tool execution layer: a command matching
any pattern is rejected with a JSON error response. See
[README](../README.md#security) for the recommended patterns.

---

## Runtime Lifecycle & Dependency Injection

`Runtime` is not a singleton. `main()` constructs a single instance and owns every
major collaborator as a `unique_ptr` member:

```
main()
 ├─ pu::Runtime runtime;
 ├─ RunAsk / RunChat(runtime)
 └─ catch (std::exception&) → friendly message
```

Key responsibilities:

- `Initialize(config_path)` — loads `agents.json` (from `./.pu/` or `~/.pu/`), creates `AgentManager`, `Executor`, `CommandRouter`, builds the default toolbox, then restores the single session from `<data-dir>/session.json` if present.
- `ProcessInput(input, ...)` — routes either to `CommandRouter` (commands) or to `Executor` (messages), then saves the session.
- `Shutdown()` — saves the single session to `<data-dir>/session.json`.
- `SwitchAgent(agent)` — updates the active agent and rebuilds the toolbox.

### Web server lifecycle

`pu serve` runs the same `Runtime` instance behind a Boost.Beast front-end
(`RunServe` in `src/app/serve.cpp`):

1. `Runtime::Initialize()` loads `agents.json` and restores the session, then the
   server mounts the static `web/` UI and registers the HTTP routes.
2. An Asio `io_context` drives the `tcp::acceptor` on its own thread; each
   accepted connection is handled on a detached thread.
3. A plain HTTP request is dispatched to the static/REST routes; a request with a
   WebSocket upgrade on `/ws` is accepted and replaces any previously active
   WebSocket session.
4. The WebSocket worker reads JSON messages: `{"type":"run","payload":{"text":"..."}}`
   spawns a worker thread that runs `Runtime::ProcessInput` under the shared
   `io_mutex`; `{"type":"cancel"}` flips the active `CancelToken`.
5. Frames are written back over the socket as the run progresses (streamed
   chunks, tool start/end, completion, or error). The frame schema is documented
   in [README](../README.md#websocket-protocol).
6. REST endpoints (`/api/session`, `/api/history`, `/api/agents`,
   `/api/agent/switch`, `/api/workspaces`, `/api/workspace/switch`, `/api/clear`)
   handle control and status queries. On Ctrl+C the server stops and
   `Runtime::Shutdown()` persists the session.

### Single-session auto-persistence

`Runtime` maintains a single `std::shared_ptr<Session> current_session_`. On
`Initialize()` it loads `<data-dir>/session.json` (if the file exists) via
`Session::Deserialize`. After every `ProcessInput()` call and on `Shutdown()`, the
session is serialized back to the same file via `Session::Serialize`. There is no
manual save/load/list/export; persistence is fully automatic and scoped to the
data directory.

### Toolbox & MCP lifecycle

```
RebuildToolbox(agent)
 ├─ ShutdownMCP()                      // stop all MCP child processes
 ├─ toolbox_ = new Toolbox()
 ├─ RegisterBuiltinTools()
 ├─ for each mcp_servers:
 │    StartMCP(cfg) → ListTools() → register mcp.<server>.<tool>
 └─ executor_->SetToolbox(toolbox_);
     executor_->SetSecurityPolicy(agent.security)
     executor_->SetCompactionConfig(agent.compaction)
```

---

## Streaming & Cancellation

### Cancel token

A `CancelToken` (`std::shared_ptr<std::atomic<bool>>`) is threaded through the
whole request stack — `Runtime::ProcessInput` → `Executor::Execute` →
`LLMProvider::Chat` → `HttpClient::PostStream`. Providers poll the flag between
chunks and stop early, so a cancellation surfaces quickly instead of waiting for
the model to finish. In `pu serve` the token is owned by the active WebSocket
session: a `{"type":"cancel"}` message — or a dropped connection — sets it, and
`BeastHttpClient` aborts the in-flight HTTP request on the next poll.

### WebSocket streaming

Chat runs exclusively over the `/ws` WebSocket. A `{"type":"run"}` message runs
`ProcessInput` on a detached worker thread; its `content_callback` writes each
streamed chunk to the socket as it arrives, which produces the typewriter effect
in the browser. The `ToolCallbacks` passed alongside it emit tool start/end events
keyed by the tool call `id`. Commands and non-streaming backends deliver their
full text as a single chunk frame. The frame schema and client-side rendering
are documented in [README](../README.md#websocket-protocol) and implemented in
`web/app.js`.

## Provider Differences

`config::CreateBackend` (`agent_config.cpp`) maps `BackendType` (`agent_config.hpp`)
to a concrete provider, and `Session::CreateProvider()` is its only caller. The
`agents.json` backend `type` field selects it, and any value other
than `"openai"` deserialises to Ollama. "OpenAI compatible" means the
`/chat/completions` SSE contract and covers OpenAI, DeepSeek thinking mode, vLLM
and compatible gateways.

| Dimension | Ollama | OpenAI compatible |
|-----------|--------|-------------------|
| Endpoint | `{host}/api/chat` | `{host}/chat/completions` |
| Auth | `Authorization: Bearer` only when a key is set | same |
| Streaming | NDJSON, one object per line, ends at `{"done":true}` | SSE, `data: ` lines, ends at `data: [DONE]` |
| Model / temperature | `model`, `options.temperature` | `model`, `temperature` |
| Token cap | not sent | `max_tokens` |
| Extra options | `keep_alive` (default `30m`, keeps the KV cache warm) | `extra_body.thinking.type = "disabled"` when `enable_thinking` is false |
| Role mapping | `user`/`assistant`/`system`/`tool`; anything else falls back to `user` | `tool_result` rewritten to `tool`; others verbatim |
| Assistant with tool calls | `content` sent as-is | `content` forced to `null` |
| Reasoning on request | never sent | sent on assistant messages when non-empty |
| Tool result fields | `role`, `tool_name`, `tool_call_id` | `role`, `tool_call_id` |
| `tool_calls.arguments` | JSON object; a string is parsed, non-JSON passed through | JSON string; an object or array is re-serialised |
| Call assembly | one complete call per line | `index`-keyed deltas flushed on `done` |
| Reasoning on response | not parsed; `IsThinkingMode()` is false | `delta.reasoning_content` accumulated |
| Usage | not parsed | logged at `trace` from `usage` |

| Capability | Ollama | OpenAI compatible |
|-----------|--------|-------------------|
| Tools, streaming content, parallel calls | yes | yes |
| Streaming tool calls | whole call per line | index accumulation |
| Reasoning | no | yes |
| Reasoning signature | no | no |
| Raw provider JSON retained | no | no |
| Multimodal input or output | no | no |
| Prompt caching hints | `keep_alive` only | none |

Both report `SupportsTools() == true`, and tool schemas fall back to `{}` via
`ToolDefinition::Parameters()`. `BackendConfig::parameters_as_string` exists but is
unread: `OpenAIProvider` always encodes `arguments` as a JSON string.

An Anthropic provider would need a `system` request field instead of a system
message, `tools[].input_schema` instead of `parameters`, `tool_use`/`tool_result`
content blocks instead of role messages, and `thinking` blocks whose `signature`
must be echoed back verbatim.

## Data Flow

```
User Input
    │
    ▼
Runtime.ProcessInput(input, ...)
    │
    ├── Is command? ──► CommandRouter ──► Session mutation
    │
    └── Is message? ──► Session.CreateProvider()
                         │
                         ▼
                       Executor.Execute()          (session-state-free)
                         │
                         ├── Inject system context into chat history
                         ├── Read Workspace.Transcript
                         ├── LLMProvider.Chat()
                         ├── Toolbox.ExecuteTool() → JSON response
                         ├── Extract std/error → store in Transcript
                         ├── Repeat tool loop if tool calls present
                         └── Return final response
                         │
                         ▼
                       Session::Serialize() → session.json
```

### Web request (streaming)

```
Browser ──WebSocket (/ws)──► RunServe handler
     │  WebSocket connection established
     ▼
Client sends: {"type":"run","payload":{"text":"..."}}
     │
     ▼
Worker thread: Runtime.ProcessInput(..., content_callback)
     │  content_callback → ws->write({"type":"chunk","payload":{"text":"..."}})
     ▼
Browser: WebSocket onmessage → parse JSON → append token to Markdown renderer

Cancellation:
Client sends: {"type":"cancel"} → CancelToken set → Beast HTTP client aborts
```

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
│          pu::mcp::Transport (interface)      │  ← Start / Stop / WriteLine
└──────────────────────┬───────────────────────┘
                       │
        ┌──────────────┴──────────────┐
        ▼                             ▼
┌──────────────────────────────────────┐   ┌──────────────────────────────────────┐
│  StdioTransport                     │   │  HttpTransport                       │
│  Child process stdio, line JSON     │   │  BeastHttpClient POST, line JSON     │
└──────────────────────────────────────┘   └──────────────────────────────────────┘
```

MCP servers are configured per-agent via `mcp_servers`. The transport is selected
automatically in `McpClient::Connect()`: a non-empty `url` selects the remote
`HttpTransport`, otherwise the stdio subprocess transport is spawned. When an
agent becomes active, `Runtime::RebuildToolbox` starts its servers, performs the
handshake, lists tools, and registers them with a `mcp.<server>.` prefix.

---

## Transcript Compaction

A request carries a `KeepRecent{head, tail}` selection, which `BuildRequestPath`
applies while rendering: the first `head` and last `tail` nodes are sent, the
omitted middle becomes a marker message, and nothing is removed from storage. A
later request can therefore carry the whole conversation again, and the boundary
moves back while it would otherwise cut off a tool call whose result has not
arrived.

`Executor::RequestSelection` supplies the policy when compaction is enabled in the
agent config and the provider is not in thinking mode (otherwise a warning is
logged). It requires a provider that does **not** support tools, and `RunToolLoop`
returns early in exactly that case, so no request currently trims. `history_compaction`
in `agents.json` is parsed and plumbed but does not take effect.

---

## Persistence

```
<data-dir>/session.json   # Single session state
```

The session file carries `schema_version` (currently 2) beside `workspace` and
`runtime_spec`. It is written automatically after every interaction and on
shutdown, and restored on startup.

Version 2 stores the conversation as a DAG: `workspace.history` holds `nodes`,
each with its id, timestamp, parents and one role payload, plus the `leaf` that
marks the current position. A file without the version, with another one, or whose
history is not node storage is refused rather than guessed at; `pu` reports the
reason, names `<data-dir>/session.v1.backup.json` when that backup exists, and
starts a fresh conversation. The older layout is a flat list of messages, which is
what the stage 0 backup preserves.

The session file contains no system prompt: the prompt is configuration and is
read from `agents.json` on every start.

---

## Directory Structure

The tree is layered bottom-up: `core/` holds dependency-free base utilities,
the domain modules sit beside it, and the orchestration headers/modules live at
the root of `include/pu/` and `src/`.

```
include/pu/
├── agent_config.hpp      # AgentConfig types + helpers
├── agent_manager.hpp     # AgentManager
├── command_router.hpp    # CommandRouter
├── runtime.hpp           # Runtime
├── executor.hpp          # Executor (session-state-free, with system context injection)
├── cli.hpp               # CLI helpers
├── core/                 # Base layer: no dependencies, no domain knowledge
│   ├── cancel_token.hpp  # Shared cancellation token (transport-agnostic)
│   ├── error.hpp         # RuntimeError / Error / HttpError
│   ├── json.hpp          # Boost.JSON convenience helpers
│   ├── logging.hpp       # spdlog setup + JSON log formatter
│   ├── path_utils.hpp    # Data-directory resolution (PU_HOME / .pu)
│   ├── platform.hpp      # OS/kernel probing, subprocess output capture
│   ├── text.hpp          # UTF-8 validation and repair
│   └── uuid.hpp          # RFC 4122 v4 identifier
├── context/              # Message model: nodes, payloads, message graph
├── infra/                # Adapters
│   ├── http_client.hpp   # HttpClient interface
│   └── beast_http_client.hpp  # Beast implementation header (impl in src/infra)
├── llm/                  # LLMProvider, providers, projection, streaming parser
├── mcp/                  # McpClient, JsonRpcClient, Transport interface, StdioTransport
├── session/              # Session, Workspace, Transcript, request view, Memory
└── tools/                # Toolbox, built-in tools, MCP adapter, tool_result

src/
├── app/                  # Entry points: main, CLI parsing, serve (web server)
│   ├── main.cpp
│   ├── cli.cpp
│   ├── serve.cpp              # RunServe: acceptor, dispatch, lifecycle
│   ├── serve_http_routes.cpp  # Static files + REST handlers
│   ├── serve_websocket.cpp    # /ws upgrade + chat frame protocol
│   └── serve_internal.hpp     # Declarations shared by the serve modules
├── agent_config.cpp, agent_manager.cpp
├── runtime.cpp, command_router.cpp
├── executor.cpp
├── core/                 # Base layer: logging, platform
├── infra/                # BeastHttpClient (network adapter)
├── llm/                  # Providers, streaming parser
├── mcp/                  # MCP transport implementations, JSON-RPC, client
├── session/              # Session, Workspace, etc.
└── tools/                # Toolbox, tools
```

A header lives in `include/pu/` when code outside its own directory uses it
(including tests); otherwise it stays next to its `.cpp`. The CMake targets
follow the layering: `pu_core` is the *base* static library (core + domain
modules), `pu_agent` holds the orchestration layer, `pu_app` holds `src/app/`,
and the `pu` executable adds only `main.cpp`.

---

## Extension Points

- **New backend**: Implement `LLMProvider` and register it in `config::CreateBackend()`.
- **New tool**: Inherit `pu::Tool`, implement methods, register in `Runtime::RegisterBuiltinTools()`.
- **New command**: Add handler in `CommandRouter`, route, update help.
- **External tool (no C++)**: Add an `mcp_servers` entry to `agents.json` — tools are discovered automatically when the agent becomes active.

---

## Known Limitations

- MCP stdio transport supports both POSIX (`fork`/`execvp`) and Windows (`CreateProcess` + pipes); the HTTP transport uses BeastHttpClient (Boost.Beast) and works on both platforms.
- MCP request timeout fixed at 5 seconds.
- Multiple `mcp_servers` entries per agent are fully supported; each server is started as a separate client and its tools are registered with the `mcp.<server_name>.` prefix.
- Compaction only supports truncation; `"summarize"` strategy is reserved.
- Environment probing uses `uname` on POSIX (kernel API on Windows), which may not be available on all systems (e.g. minimal containers). It fails gracefully and falls back to `"unknown"`.

---

## License

GPL-3.0 — see [LICENSE](../LICENSE)
