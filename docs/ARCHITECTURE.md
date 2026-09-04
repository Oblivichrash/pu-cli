# pu-cli Architecture

> "朴散则为器"——《老子》

## Overview

pu-cli is built around four principles:

1. **Single-session auto-persistence** — One session owns the workspace and history, transparently persisted.
2. **Dynamic backend switching** — Switch LLM providers without losing state.
3. **Stateless execution** — `Executor` holds no state; all state lives in `Workspace`/`Session`.
4. **Explicit composition** — `Runtime` is a plain object instantiated by `main()` and injected with its collaborators; there is no global singleton.

---

## Core Components

| Component | Responsibility |
|-----------|----------------|
| `Runtime` | Plain object created by `main()`; owns `AgentManager`, `Toolbox`, `Executor`, `CommandRouter`; routes input, holds the single `Session`, rebuilds tool registry on agent switch |
| `Session` | Aggregate root: `Workspace` + `RuntimeSpec` |
| `Workspace` | State container: `Transcript` (history) + `Memory` (variables/artifacts) |
| `Executor` | Stateless tool loop; reads/writes `Workspace`; injects system context and processes structured tool output |
| `LLMProvider` | Model gateway; handles transport + format adaptation |
| `Toolbox` | Tool registry; rebuilt per active agent, executes built-in and MCP tools |
| `CommandRouter` | Routes `/` commands to handlers |
| `Web Server` | `pu serve` (`RunServe` in `src/app/serve.cpp`): cpp-httplib HTTP server exposing the session as a JSON/SSE API and mounting the `web/` UI |
| `McpClient` | High-level MCP client: handshake, `ListTools`, `CallTool` |
| `JsonRpcClient` | JSON-RPC 2.0 protocol layer |
| `StdioTransport` | stdio subprocess transport |
| `HttpTransport` | remote streamable-HTTP transport (curl POST, line-delimited responses) |
| `ArtifactExtractor` | Extracts `Artifact`s from workspace history |

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

## Executor Enhancements (since v0.4)

### Structured Tool Output

Every tool (`execute_bash`, `write_file`, MCP tools) returns a JSON object with the following schema:

```json
{
  "success": bool,
  "stdout": string,
  "stderr": string,
  "error": string,
  "exit_code": int
}
```

The `Executor` extracts `stdout` (if `success==true`) or `error` (if `success==false`) and stores only that content in the transcript. The full JSON is not persisted, keeping history clean and human‑readable.

### System Context Injection

`Executor` automatically builds a system message containing:

- OS name and kernel version (probed once at startup)
- Available system tools (detected via `which`)
- Security policy (sandbox root, forbidden patterns)
- Current working directory (the sandbox root)
- Last known file paths (extracted from artifacts)
- Recent tool executions (up to 2) with success/failure status and truncated output

This context is merged with the user‑defined `system_prompt` (if any) and prepended to the chat history on every request. This gives the model full awareness of its environment, dramatically reducing blind attempts.

### Environment Probing

`Executor::ProbeStaticEnvironment()` runs once during construction and uses `uname -s`, `uname -r`, and `which` to detect the OS and common tools (`bash`, `python3`, `gcc`, `git`, `curl`, `jq`). The result is cached and included in the system context.

### Forbidden Patterns

The security policy's `forbidden_patterns` is enforced at the tool execution layer. Commands matching any pattern are rejected with a JSON error response. It is strongly recommended to include `"cd"` to prevent the model from changing the working directory.

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

`pu serve` runs the same `Runtime` instance behind an HTTP front-end
(`RunServe` in `src/app/serve.cpp`):

1. `Runtime::Initialize()` loads `agents.json` and restores the session, then the
   server mounts the static `web/` UI and registers the API routes.
2. All chat handlers serialize on a single `io_mutex` — `ProcessInput` mutates
   the one persistent `Session`, so the lock is shared with the CLI paths.
3. `POST /api/chat/stream` registers a `CancelToken` under a `request_id`, spawns
   a worker thread that runs `ProcessInput`, and returns a chunked SSE response
   that flushes tokens as they arrive.
4. `POST /api/chat/cancel` flips the `CancelToken` for a `request_id`; the entry
   is erased from `active_requests` once processing finishes (or the client
   disconnects).
5. `svr.listen()` runs on its own thread; on Ctrl+C the server stops and
   `Runtime::Shutdown()` persists the session.

### Single-session auto-persistence

`Runtime` maintains a single `std::shared_ptr<Session> current_session_`. On
`Initialize()` it loads `<data-dir>/session.json` (if the file exists) via
`Session::Deserialize`. After every `ProcessInput()` call and on `Shutdown()`, the
session is serialized back to the same file via `Session::Serialize`. There is no
manual save/load/list/export; persistence is fully automatic and scoped to the data
directory (`PU_HOME` or `./.pu/`).

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
the model to finish. `POST /api/chat/cancel` looks up the token registered under
the client-supplied `request_id` and sets it; the SSE provider also sets the same
token when it detects that the client disconnected.

### SSE streaming

`POST /api/chat/stream` answers with `Content-Type: text/event-stream` and
`Cache-Control: no-cache`. `ProcessInput` runs on a worker thread and its
`content_callback` pushes `data: {"token": "<chunk>"}\n\n` events into a shared
`SseStream` queue; a cpp-httplib chunked content provider
(`Response::set_chunked_content_provider`) drains the queue on the connection
thread and writes each event to the socket as it arrives, which is what produces
the typewriter effect in the browser. Completion is signalled with
`data: [DONE]\n\n`, failures with `data: {"error": "..."}\n\n`, and commands or
non-streaming backends deliver their full text as a single token event. The
front-end (`web/app.js`) parses the stream line by line and falls back to the
non-streaming `POST /api/chat` endpoint on HTTP/network errors or when
`ReadableStream` is unsupported.

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
                       Executor.Execute()          (stateless)
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
Browser ──POST /api/chat/stream──► RunServe handler
     │  register CancelToken in active_requests
     ▼
Worker thread: Runtime.ProcessInput(message, ..., content_callback)
     │  content_callback → PushSseEvent("data: {\"token\": ...}\n\n")
     ▼
SseStream queue (mutex + condition_variable)
     │  drained by httplib chunked content provider on the connection thread
     ▼
Browser: ReadableStream → parse SSE → append token to pending message
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
│  Child process stdio, line JSON     │   │  CurlHttpClient POST, line JSON      │
└──────────────────────────────────────┘   └──────────────────────────────────────┘
```

MCP servers are configured per-agent via `mcp_servers`. The transport is selected
automatically in `McpClient::Connect()`: a non-empty `url` selects the remote
`HttpTransport`, otherwise the stdio subprocess transport is spawned. When an
agent becomes active, `Runtime::RebuildToolbox` starts its servers, performs the
handshake, lists tools, and registers them with a `mcp.<server>.` prefix.

---

## Transcript Compaction

`Transcript::Compact(keep_head, keep_tail)` keeps the head and tail messages and discards the middle. It preserves tool‑call pairing by scanning backward and ensuring all tool‑call IDs have matching responses.

Compaction runs automatically in `Executor::Execute()` if:
- The provider does not support tools (or `tools` list is empty)
- Compaction is enabled in the agent config
- The provider is **not** in thinking mode (otherwise compaction is skipped and a warning is logged)

---

## Persistence

```
<data-dir>/session.json   # Single session state
```

The session file contains serialized `Workspace` and `RuntimeSpec`. It is written
automatically after every interaction and on shutdown, and restored on startup.

---

## Directory Structure

Single-file modules live directly under `include/pu/` and `src/`; directories are kept
only where a module has multiple files.

```
include/pu/
├── agent_config.hpp      # AgentConfig types + helpers
├── agent_manager.hpp     # AgentManager
├── command_router.hpp    # CommandRouter
├── runtime.hpp           # Runtime
├── executor.hpp          # Executor (stateless, with system context injection)
├── http_client.hpp       # HttpClient interface
├── cli.hpp, error.hpp, path_utils.hpp
├── core/                 # Logging
├── infra/                # Platform utilities
├── llm/                  # LLMProvider, Ollama/OpenAI providers, streaming parser
├── mcp/                  # McpClient, JsonRpcClient, StdioTransport, HttpTransport
├── session/              # Session, Workspace, Transcript, Memory
└── tools/                # Toolbox, built-in tools, MCP adapter, tool_result

src/
├── app/                  # CLI entry (main), UI helpers
├── agent_config.cpp, agent_manager.cpp
├── runtime.cpp, command_router.cpp
├── executor.cpp
├── core/                 # Logging
├── infra/                # CurlHttpClient, platform
├── llm/                  # Providers, streaming parser
├── mcp/                  # MCP transport, JSON-RPC, client
├── session/              # Session, Workspace, etc.
└── tools/                # Toolbox, tools
```

---

## Extension Points

- **New backend**: Implement `LLMProvider` and register in `Session::CreateProvider()`.
- **New tool**: Inherit `pu::Tool`, implement methods, register in `Runtime::RegisterBuiltinTools()`.
- **New command**: Add handler in `CommandRouter`, route, update help.
- **External tool (no C++)**: Add an `mcp_servers` entry to `agents.json` — tools are discovered automatically when the agent becomes active.

---

## Known Limitations

- MCP stdio transport supports both POSIX (`fork`/`execvp`) and Windows (`CreateProcess` + pipes); the HTTP transport uses libcurl and works on both platforms.
- MCP request timeout fixed at 5 seconds.
- Multiple `mcp_servers` entries per agent are fully supported; each server is started as a separate client and its tools are registered with the `mcp.<server_name>.` prefix.
- Compaction only supports truncation; `"summarize"` strategy is reserved.
- Environment probing uses `uname` and `which`, which may not be available on all systems (e.g., minimal containers). It gracefully fails and logs a warning.

---

## License

GPL-3.0 — see [LICENSE](../LICENSE)
