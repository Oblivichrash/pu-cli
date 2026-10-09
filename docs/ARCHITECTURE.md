# pu-cli Architecture

> "朴散则为器"——《老子》

## Overview

`Runtime` is the only object `main()` creates; everything else is injected into it
and holds no global state. A single session owns the conversation and is persisted
automatically, and the backend behind it can change without losing that state. The
components below carry the rest.

---

## Tech Stack

What each dependency is for:

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
| `Workspace` | State container: `Transcript` (history) |
| `Executor` | Session-state-free tool loop (holds config + probe cache); reads/writes `Workspace`; injects system context and processes structured tool output |
| `LLMProvider` | Model gateway; handles transport + format adaptation |
| `Toolbox` | Tool registry; rebuilt per active agent, executes built-in and MCP tools |
| `CommandRouter` | Routes `/` commands to handlers |
| `Web Server` | `pu serve` (`RunServe`): Boost.Beast HTTP/WebSocket server exposing the session via `/ws` for chat and REST for control/status |
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
(`boost::json::value`). `include/pu/core/json.hpp` is a thin convenience layer over
it: `parse`, `serialize`, `ValueOrDefault`, `HasKey` and `PrettyPrint`. Each one
carries its own contract where it is declared, and the helper is what to reach for
rather than the raw Boost call, so the fallbacks stay in one place.

JSON is used for configuration (`agents.json`), session persistence
(`Session::Serialize` / `Session::Deserialize`), structured tool output
(`pu::tools::tool_result.hpp`), the MCP JSON-RPC layer, and the WebSocket/REST
API in `src/app/serve_http_routes.cpp` and `src/app/serve_websocket.cpp`.

---

## Executor

The tool loop holds no session state (`main()` injects its collaborators) and
runs once per turn: read `Workspace.Transcript`, inject system context, call the
provider, execute any tool calls through `Toolbox`, store each structured result
verbatim, and repeat until the reply carries no call.

**System context.** `Executor` prepends a system message built from:

- OS name and kernel version (probed once, see below)
- Security policy (sandbox root, forbidden patterns)
- Current working directory (the sandbox root)
- Tool-use guidelines for the model

It is merged with the agent's configured `system_prompt`, which comes from
`agents.json` via `Runtime::RebuildToolbox` and is not session state, so
switching the backend does not clear it.

**Structured tool output.** Tools return JSON through the schema in
`include/pu/tools/tool_result.hpp` (documented in
[README](../README.md#tool-output-format)). The executor stores it verbatim so
the model sees what the tool produced, and reads `stdout`/`error` out of it only
for the tool callbacks.

**Environment probing.** `Executor::ProbeStaticEnvironment()` runs once during
construction (`uname` on POSIX, the Windows kernel API elsewhere) and caches the
result. No tool-binary detection is performed.

**Forbidden patterns.** `forbidden_patterns` is enforced at the tool execution
layer: a matching command is rejected with a JSON error. The policy fields live in
[README](../README.md#agentsjson).

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

- `Initialize(config_path)` — loads `agents.json` (from `./.pu/` or `~/.pu/`), creates `AgentManager`, `Executor`, `CommandRouter`, builds the default toolbox, then restores the single session from `<workspace>/.pu/session.json` if present.
- `ProcessInput(input, ...)` — routes either to `CommandRouter` (commands) or to `Executor` (messages), then saves the session.
- `Shutdown()` — saves the single session to `<workspace>/.pu/session.json`.
- `SwitchAgent(agent)` — updates the active agent and rebuilds the toolbox.

### Web server lifecycle

`pu serve` runs the same `Runtime` instance behind a Boost.Beast front-end
(`RunServe` in `src/app/serve.cpp`), on the host and port that the command line, the
environment, or the workspace's own `serve` block names:

1. `Runtime::Initialize()` loads `agents.json` and restores the session, then the
   server mounts the static `web/` UI and registers the HTTP routes.
2. An Asio `io_context` drives the `tcp::acceptor` on its own thread; each
   accepted connection is handled on a detached thread.
3. A plain HTTP request is dispatched to the static/REST routes; a WebSocket upgrade
   on `/ws` is accepted as the client being written to. A second one while that client
   is attached is answered with a `busy` frame and closed instead: one session, one
   page.
4. The WebSocket worker reads JSON messages: `{"type":"run","payload":{"text":"..."}}`
   spawns a worker thread that runs `Runtime::ProcessInput` under the shared
   `io_mutex`; `{"type":"cancel"}` flips the active `CancelToken`.
5. Frames are written back over the socket as the run progresses (streamed
   chunks, tool start/end, completion, or error). The frame schema is documented
   in [README](../README.md#web-api).
6. REST endpoints handle control and status queries; the list is in
   [README](../README.md#web-api). On Ctrl+C the server stops and
   `Runtime::Shutdown()` persists the session.

### Single-session auto-persistence

`Runtime` maintains a single `std::shared_ptr<Session> current_session_`. On
`Initialize()` it loads `<workspace>/.pu/session.json` (if the file exists) via
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
```

---

## Streaming & Cancellation

### Cancel token

A `CancelToken` threads through the request stack from `Runtime::ProcessInput`
down to the HTTP stream; the provider polls it between chunks and the transport
between reads of the body, so a cancellation surfaces quickly even from a stream
that has gone quiet. In `pu serve` it is set by a `cancel` message or by the client
watching the turn going away, so closing the page stops the reply as surely as the
Stop button does. The turn then ends with neither a reply nor an error.

### HTTP streaming

A provider's answer arrives as an SSE body the producer writes over the life of the
request, and `BeastHttpClient::StreamResponse` reads it the way it is written: the
header first, then the body in bounded pieces, each handed to the line parser as it
lands. Reading the body to its end before parsing any of it collapses the stream into
one delivery at the end, and every layer above is then streaming in name only — a
page cannot show a reply growing, and a stop has nothing left to interrupt. The
failure path is the exception: an error body is collected whole, because the message
that says what went wrong is in it.

### WebSocket streaming

Chat runs exclusively over the `/ws` WebSocket. A `{"type":"run"}` message runs
`ProcessInput` on a detached worker thread; its `content_callback` writes each
streamed chunk to the socket as it arrives, which produces the typewriter effect
in the browser. The `ToolCallbacks` passed alongside it emit tool start/end events
keyed by the tool call `id`. Commands and non-streaming backends deliver their
full text as a single chunk frame. The frame schema and client-side rendering
are documented in [README](../README.md#web-api) and implemented in
`web/app.js`.

A turn belongs to the client watching it. Closing the page, losing the socket, or a
second client taking over all end it, and nothing of it is kept: what it had written
is half an answer, and storing that would make the next request read it as the model's
finished reply. The chat is therefore never driven by nobody, and a page that reloads
mid-answer comes back to the conversation as the store has it — the question, and no
reply to it.

## Provider Differences

What each backend puts on the wire is reference material, not architecture, and
lives in [providers.md](providers.md).

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
                         ├── Store the tool result in Transcript
                         ├── Repeat tool loop if tool calls present
                         └── Return final response
                         │
                         ▼
                       Session::Serialize() → session.json
```

Each stored node is rendered into one `ChatMessage` on the way out
(`src/session/request.cpp`), which is the view a provider requires. That makes
`ChatMessage` a compatibility view rather than a place to grow: a new context
feature belongs to `MessageNode` (`include/pu/context/message.hpp`), which owns what
a turn is.

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

A client that leaves — closing the page, reloading, losing the connection — does the
same thing, and a second client taking over does it for the first: a turn is only
ever written for the reader who asked for it.
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

MCP servers are configured per-agent via `mcp_servers`, and a list may hold any
number of them: each is started as its own client, and its tools are registered
under the `mcp.<server>.` prefix. The transport is selected automatically in
`McpClient::Connect()`: a non-empty `url` selects the remote `HttpTransport`,
otherwise the stdio subprocess transport is spawned. Stdio runs a child process
on both POSIX (`fork`/`execvp`) and Windows (`CreateProcess`), and HTTP goes
through `BeastHttpClient`, so both work on every platform. When an agent becomes
active, `Runtime::RebuildToolbox` starts its servers, performs the handshake, and
lists tools.

---

## Persistence

```
<workspace>/.pu/session.json   # Single session state
```

`<workspace>` is the directory `pu` was started in (`Runtime::workspace_root_`),
which a workspace switch can move.

The session file carries `schema_version` (currently 4) beside `workspace` and
`runtime_spec`. It is written automatically after every interaction and on
shutdown, and restored on startup.

`runtime_spec` names the agent and carries a backend only when `/backend` gave
this session one of its own. Every other backend field is read from
`agents.json` on each start, so editing the configuration takes effect without
touching the session. The session also decides which agent a restart resumes:
the named agent wins over `default_agent`.

The conversation is a chain: `workspace.history` holds `nodes`, each with its id,
timestamp, its parent and one role payload, plus the `leaf` that marks the current
position. A payload's `content` is a single string, and reasoning is the JSON the
provider sent (`reasoning.raw_json`).

`/rewind` moves the `leaf` back and removes nothing, so the turns after it stay
in the file until something replaces them. The append that follows drops whatever
the new leaf cannot reach, so a replaced turn leaves nothing behind and the store
ends up holding exactly the chain the view shows.

A file without the version, with another one, or whose history is not node storage is
refused rather than guessed at: `pu` reports the reason and starts a fresh
conversation, and the next save replaces the file. This is a prototype, so nothing is
kept for an older layout — no conversion, and no copy taken aside.

The session file contains no system prompt: the prompt is configuration and is
read from `agents.json` on every start.

---

## Directory Structure

The tree is layered bottom-up: `core/` holds dependency-free base utilities,
the domain modules sit beside it, and the orchestration headers/modules live at
the root of `include/pu/` and `src/`.

```
include/pu/                  src/
├── agent.hpp                ├── app/                  # main, CLI parsing, serve
├── command_router.hpp       ├── agent_config.cpp, agent_manager.cpp
├── runtime.hpp              ├── runtime.cpp, command_router.cpp
├── executor.hpp             ├── executor.cpp
├── cli.hpp                  ├── core/                 # logging, platform, HTTP client
├── core/                    ├── context/              # message graph storage
├── context/                 ├── llm/                  # providers, streaming parser
├── llm/                     ├── mcp/                  # transports, JSON-RPC client
├── mcp/                     ├── session/              # Session, Workspace
├── session/                 └── tools/                # Toolbox, tools
└── tools/
```

A header lives in `include/pu/` when code outside its own directory uses it
(including tests); otherwise it stays next to its `.cpp`. The CMake targets
follow the layering: `pu_core` is the *base* static library (core + domain
modules), `pu_agent` holds the orchestration layer, `pu_app` holds `src/app/`,
and the `pu` executable adds only `main.cpp`.

---

## Extension Points

- **New backend**: Implement `LLMProvider` and register it in `CreateBackend()` (`include/pu/agent.hpp`).
- **New tool**: Inherit `pu::Tool`, implement methods, register in `Runtime::RegisterBuiltinTools()`.
- **New command**: Add handler in `CommandRouter`, route, update help.
- **External tool (no C++)**: Add an `mcp_servers` entry to `agents.json` — tools are discovered automatically when the agent becomes active.

---

## Known Limitations

- MCP requests time out after a fixed 5 seconds.
- Environment probing can come up empty: an absent `uname` on POSIX yields nothing, and a failed Windows kernel API falls back to `"unknown"`.
- **Nothing enforces a token budget, by design.** `ChatResult::usage` carries what the provider counted, and the executor logs it at `debug`, but no limit is compared against it, so a conversation still grows until the provider refuses it and the refusal reaches the user as an HTTP error.
- **A cancelled run keeps no partial reply, by design.** The transport aborts the stream and the executor ends the turn with neither a reply nor an error, so nothing is appended: the session holds the user message and no answer, and a follow-up "continue" restarts the answer rather than resuming it.
- **The store is only persisted after a completed interaction and on shutdown.** A crash loses everything since the last save, and the store is held in memory in between.

