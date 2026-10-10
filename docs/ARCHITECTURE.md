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
| `Session` | Aggregate root: a `Conversation` + its `SessionSpec` |
| `Conversation` | The stored conversation: the `MessageGraph`, plus the `ChatMessage` view rendered from it |
| `Executor` | Session-state-free tool loop (holds config + probe cache); reads and writes the `Conversation`; injects system context and processes structured tool output |
| `LLMProvider` | Model gateway; handles transport + format adaptation. `StreamingProvider` implements the shared stream pipeline; a concrete backend fills in the endpoint, headers, and per-line parse |
| `Toolbox` | Tool registry with a fixed address; emptied and refilled per active agent, executes built-in and MCP tools |
| `CommandRouter` | Routes `/` commands to handlers |
| `Web Server` | `pu serve` (`RunServe`): Boost.Beast HTTP/WebSocket server exposing the session via `/ws` for chat and REST for control/status |
| `McpClient` | High-level MCP client: handshake, `ListTools`, `CallTool` |
| `mcp_session` | Free functions that connect a server set and disconnect it again — the whole connect/disconnect sequence, so `Runtime` holds the result rather than driving the loop |
| `JsonRpcClient` | JSON-RPC 2.0 protocol layer |
| `Transport` | Abstract MCP transport (`Start` / `Stop` / `WriteLine`) |
| `StdioTransport` | stdio subprocess transport |
| `HttpTransport` | remote streamable-HTTP transport (BeastHttpClient POST, line-delimited responses) |

---

## Error Handling

A condition is checked before it happens when the code can know it: an operation that
can fail visibly returns `std::optional` or `nullptr` rather than throwing. Exceptions
are for what cannot be known in advance — a body that does not parse, a peer that
closes the socket, a provider that answers with an error frame.

All non-recoverable runtime errors derive from a single base class:

```
pu::RuntimeError : std::runtime_error
  ├── pu::RequestRefused        (the state cannot serve a well-formed request)
  └── pu::Error                 (the request or the configuration is wrong)
        └── pu::HttpError       (HttpClient failures)
```

`pu::RequestRefused` sits beside `Error`, not under it, because a `pu serve`
route reports the two differently: a refused request answers 400, and an `Error`
answers 500. Deriving a refusal from `Error` would report a caller's mistake as a
fault of this process.

`main()` wraps top-level dispatch in a `try/catch (const std::exception&)` so any
`RuntimeError` is converted to a friendly fatal-error message.

### What a catch is for

A catch is worth its place only when it changes the control flow: it returns a value,
retries, falls back, converts, or ends the process. Logging is not handling, but it is
not forbidden either — what it must not be is the *whole* body of a catch on a path
that then carries on as if nothing happened.

Where a layer must report and continue, it converts rather than swallows:

- **A parse that fails is a result.** `json::parse` throwing is how a byte string says
  it is not JSON; the catch at the call site turns that into the value the caller wants
  (a default, a fallback, a marked-invalid record). This is the largest group of catches
  in the tree and none of them are noise.
- **A background thread reports and ends.** `McpClient` and the WebSocket reader catch
  at their own boundary because no caller exists above them to be told; the log is the
  report, and the promise or the socket carries the outcome.
- **The process boundary logs and stops.** `main()` and the CLI entry points are the only
  places a message may be the entire response, because returning is no longer an option.

A catch that logs and then lets the caller believe the operation succeeded is the one
shape to avoid. `McpClient::ListTools` used to be the example: it answered with an empty
list when `tools/list` failed, so a dead server looked like a server with no tools. It now
throws, because "no tools" and "the reply was not understood" are different things for a
caller to act on, and the boundary that can act on it — `Runtime::RebuildToolbox` — treats
the failure the same way it treats a server that never connected: skip it and say so.

### Configuration is a state, not a failure

A workspace without an `agents.json` is an ordinary state. `config::FindConfigPath`
returns an empty string for it, and `FindServeOptions` returns `std::nullopt`; the one
place that cannot continue, `Runtime::Initialize`, turns the empty path into the `Error`
that says where to put the file. A function that cannot answer reports that it cannot
answer, rather than picking one of its callers' conditions to throw.

The same idea applies to an invariant. `Runtime::ActiveAgent` and `ConfiguredBackend` do
not check that the runtime was initialized or that the active agent is configured, because
`Initialize` every path that reaches them must pass through already establishes both. A
check for a condition that cannot be false is not defensive programming; it is a claim
that the code is unsure of itself, and it costs a reader the work of proving it false.

---

## JSON Handling

All JSON parsing and serialization is provided by **Boost.JSON**
(`boost::json::value`). `include/pu/core/json.hpp` is a thin convenience layer over
it: `parse`, `serialize`, `ValueOrDefault`, `HasKey` and `PrettyPrint`. Each one
carries its own contract where it is declared, and the helper is what to reach for
rather than the raw Boost call, so the fallbacks stay in one place.

JSON is used for configuration (`agents.json`), session persistence
(`Session::Serialize` / `Session::Deserialize`), structured tool output
(`pu::tools::tool.hpp`), the MCP JSON-RPC layer, and the WebSocket/REST
API in `src/app/serve_http_routes.cpp` and `src/app/serve_websocket.cpp`.

---

## Executor

The tool loop holds no session state (`main()` injects its collaborators) and
runs once per turn: read the `Conversation`, inject system context, call the
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
`include/pu/tools/tool.hpp` (documented in
[README](../README.md#tool-output-format)). The executor stores it verbatim so
the model sees what the tool produced, and reads `stdout`/`error` out of it only
for the tool callbacks. `Toolbox::ExecuteTool` answers with the same envelope
when the named tool is absent, so callers never parse two shapes.

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
4. The WebSocket worker reads JSON messages, spawns a worker thread per run, and
   writes each frame back as the run progresses. The message schema, the REST route
   table, and the status each refusal carries are in
   [README](../README.md#web-api).
5. On Ctrl+C the server stops and `Runtime::Shutdown()` persists the session.

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
   ├─ DisconnectMcpServers(mcp_clients_)
   ├─ ConnectMcpServers(agent.mcp_servers) // one client per server that answered
   ├─ toolbox_.Clear()
   ├─ RegisterBuiltinTools()
   ├─ RegisterMcpTools()                   // ListTools() → register mcp.<server>.<tool>
   └─ executor_->SetSecurityPolicy(agent.security)
  ```

 `Runtime` still owns the connected `mcp_clients_`, but the connect/disconnect
 sequencing itself lives in `pu::mcp` as three free functions
 (`ConnectMcpServer`, `ConnectMcpServers`, `DisconnectMcpServers`); `Runtime`
 holds the result rather than driving the loop. Because a client remembers the
 server it was built from (`McpClient::ServerName()`), registration pairs tools
 with their server without reaching back into the vector for the last element
 pushed.

 `Toolbox` is a value member of `Runtime` and `RebuildToolbox` empties it in place rather
 than replacing it, so the registry keeps one address for the process lifetime. The
 executor still receives it by reference per `Execute` call, but the reason is ordinary
 parameter passing rather than self-defence: there is no second address to be left
 pointing at, and no uninitialized state to guard against.

 `Toolbox::RegisterTool` returns `false` instead of throwing when a name is empty or the
 tool is null. Both conditions are knowable at the call site, so the caller decides —
 `RegisterBuiltinTools` names its tools statically and ignores the result, while the MCP
 loop warns and skips the one tool whose sanitized name was rejected.

 An `McpTool` holds its client through a `shared_ptr`, so a rebuild dropping the runtime's
 reference does not pull the client out from under a tool that still refers to it. That is
 what makes the order of the first two steps above a matter of tidiness rather than
 correctness: whichever runs first, no tool outlives the connection it calls through.

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

Chat runs exclusively over the `/ws` WebSocket. A run message starts
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
                         ├── Read the Conversation
                         ├── LLMProvider.Chat()
                         ├── Toolbox.ExecuteTool() → JSON response
                         ├── Store the tool result in the Conversation
                         ├── Repeat tool loop if tool calls present
                         └── Return final response
                         │
                         ▼
                       Session::Serialize() → session.json
```

Each stored node is rendered into one `ChatMessage` on the way out
(`include/pu/session/request.hpp`), which is the view a provider requires. That makes
`ChatMessage` a compatibility view rather than a place to grow: a new context
feature belongs to `MessageNode` (`include/pu/context/message.hpp`), which owns what
a turn is.

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

`<workspace>` is the directory `pu` was started in (`Runtime::workspace_root_`).

The session file carries `schema_version` (currently 7) beside `conversation` and
`session_spec`. It is written automatically after every interaction and on
shutdown, and restored on startup.

`session_spec` names the agent and carries the session's own thinking level when
`/thinking` set one. Every backend field is read from `agents.json` on each
start, so editing the configuration takes effect without touching the session.
`/backend <type> <model>` adopts an agent named after the type, so the backend a
session reports and the agent it names are always the same answer. The session
also decides which agent a restart resumes: the named agent wins over
`default_agent`.

The conversation is a chain: `conversation.history` holds `nodes`, each with its id,
timestamp, its parent and one role payload, plus the `leaf` that marks the current
position. A payload's `content` is a single string, and reasoning is a list of blocks,
each with the provider's `text`, an optional `signature` and the provider's own `raw`
encoding when it round-trips (see [providers.md](./providers.md)).

A node's id is its own, not a position: the earlier `id = size() + 1` scheme made a
rewind representable only by rewriting the file, and a branch not at all.

`/rewind` moves the `leaf` back and removes nothing, so the turns after it stay
in the file until something replaces them. The append that follows drops whatever
the new leaf cannot reach, so a replaced turn leaves nothing behind and the store
ends up holding exactly the chain the view shows.

A file without the version, with another one, or whose history is not node storage is
refused rather than guessed at: `pu` reports the reason and starts a fresh
conversation, and the next save replaces the file.

The session file contains no system prompt: the prompt is configuration and is
read from `agents.json` on every start.

---

## Directory Structure

The tree is layered bottom-up: `core/` holds dependency-free base utilities,
the domain modules sit beside it, and the orchestration headers/modules live at
the root of `include/pu/` and `src/`.

```
include/pu/                  src/
├── agent_manager.hpp        ├── app/                  # main, CLI parsing, serve
├── command_router.hpp       ├── config/               # agents.json, the backend model
├── runtime.hpp              ├── runtime.cpp, command_router.cpp
├── executor.hpp             ├── executor.cpp
├── app/                     ├── core/                 # logging, platform, HTTP client
├── config/                  ├── context/              # message graph storage
├── core/                    ├── llm/                  # providers, streaming parser
├── context/                 ├── mcp/                  # transports, JSON-RPC client
├── llm/                     ├── session/              # Session, Conversation
├── mcp/                     └── tools/                # Toolbox, tools
├── session/
└── tools/
```

A header lives in `include/pu/` when code outside its own directory uses it
(including tests); otherwise it stays next to its `.cpp`. A header whose module
has a directory belongs in that directory, so `app/cli.hpp` sits beside
`src/app/cli.cpp`.

The CMake targets follow the layering: `pu_lib` is the *base* static library
(core + domain modules), `pu_agent` holds the orchestration layer, `pu_app`
holds `src/app/`, and the `pu` executable adds only `main.cpp`.

### Dependency direction

Includes point one way: `core/` is a leaf, `config/` and `llm/` build on it, and
`mcp/`, `tools/`, `session/`, and the orchestration headers build on those. No
directory includes a peer that sits above it, and `config/` in particular never
reaches `mcp/`.

The rule holds because a record belongs to the layer that *parses* it, not the
layer that *uses* it. `config/mcp_server.hpp` and `config/security_policy.hpp`
are leaf headers holding plain data — `McpServerConfig` (read from `agents.json`)
and `SecurityPolicy` — so `mcp/client.hpp` and `tools/tool.hpp` include them
rather than the reverse. Before the split both were declared next to their
consumers, which made `tools/` depend on `config/` depending on `mcp/`: a cycle
that pulled the whole transport layer into any header naming a `Tool`.

---

## Extension Points

- **New backend**: Inherit `StreamingProvider` (`include/pu/llm/streaming_provider.hpp`) and supply the endpoint path, request body, log tag, headers, and a per-line parser; a backend that does not stream over SSE or NDJSON inherits `LLMProvider` directly. Register it in `CreateBackend()` (`include/pu/config/backend.hpp`).
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

