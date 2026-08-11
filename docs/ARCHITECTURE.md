# pu-cli Architecture

> "朴散则为器"——《老子》

## Overview

pu-cli is built around four principles:

1. **Session isolation** — Each session owns its workspace and history.
2. **Dynamic backend switching** — Switch LLM providers without losing state.
3. **Stateless execution** — `Executor` holds no state; all state lives in `Workspace`/`Session`.
4. **Explicit composition** — `Runtime` is a plain object instantiated by `main()` and injected with its collaborators; there is no global singleton.

---

## Core Components

| Component | Responsibility |
|-----------|----------------|
| `Runtime` | Plain object created by `main()`; owns `AgentManager`, `SessionStore`, `Toolbox`, `Executor`, `CommandRouter`; routes input, manages sessions, rebuilds tool registry on agent switch |
| `Session` | Aggregate root: `Workspace` + `RuntimeSpec` |
| `Workspace` | State container: `Transcript` (history) + `Memory` (variables/artifacts) |
| `Executor` | Stateless tool loop; reads/writes `Workspace`; injects system context and processes structured tool output |
| `LLMProvider` | Model gateway; handles transport + format adaptation |
| `Toolbox` | Tool registry; rebuilt per active agent, executes built-in and MCP tools |
| `CommandRouter` | Routes `/` commands to handlers |
| `SessionStore` | Persists sessions to `~/.pu/sessions/` |
| `McpClient` | High-level MCP client: handshake, `ListTools`, `CallTool` |
| `JsonRpcClient` | JSON-RPC 2.0 protocol layer |
| `StdioTransport` | stdio subprocess transport |
| `ArtifactExtractor` | Extracts `Artifact`s from workspace history |
| `ConfigCli` | `pu config` CLI: agent CRUD (`list`/`show`/`add`/`remove`/`rename`/`set-default`), interactive wizard, `refresh-models`, `probe`; stateless free functions, no `Runtime` dependency |
| `config_tools` | Implementation modules behind `pu config`: `AgentCrud` (config file mutation), `InteractiveWizard`, `PromptTemplates`, `ModelScanner` (NIM/Ollama/OpenAI-compatible), `SystemProbe` |

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
`RuntimeError` is converted to a friendly fatal-error message.

---

## Configuration CLI (`pu config`)

`pu config` manages `agents.json` without manual editing. It is deliberately
kept independent of `Runtime`/`Executor`: `main()` dispatches `config` directly
to `RunConfig()` (see [Runtime Lifecycle](#runtime-lifecycle--dependency-injection)),
and every operation is a free function taking the resolved config path plus
explicit collaborators — no global state.

```
main()
 └─ config ──► RunConfig(argc, argv)
                  ├─ list/show/add/remove/rename/set-default ──► AgentCrud + InteractiveWizard
                  ├─ refresh-models ──► ModelScanner (HttpClient::Get)
                  └─ probe ──► SystemProbe (HttpClient::Get)
```

- `AgentCrud` reads/writes `agents.json` (located via `FindConfigPath()`,
  honoring `PU_AGENTS_CONFIG`/`PU_HOME`) and preserves unknown fields.
- `ModelScanner` probes NVIDIA NIM (`NVIDIA_API_KEY`), Ollama
  (`GET /api/tags`), and OpenAI-compatible endpoints (`OPENAI_API_KEY`).
- `SystemProbe` reports OS, kernel, arch, working dir, and provider status.
- Testability: scanning/probing functions have `HttpClient&` overloads so unit
  tests inject `MockHttpClient`; the no-arg overloads construct
  `CurlHttpClient` internally.
- `HttpClient` gained a `Get()` method (blocking, returns body) in addition to
  the existing `PostStream()`.

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

### ask_user Clarification

`ask_user` is a built-in tool the model uses when it needs information from the
user before continuing (see the Tool Use Guidelines in the static system
context). It executes through the normal tool path: `AskUserTool` validates the
`question` argument and returns a `clarification_needed` marker carrying the
question. The executor recognizes the marker in the tool result, stops the
loop, and returns the `question` as the final response without feeding the
result back to the model. The assistant's tool call, the tool result, and the
question are all recorded in the transcript, so the exchange survives session
persistence and multi-turn context.

### Environment Probing

`Executor::ProbeStaticEnvironment()` runs once during construction and uses `uname -s`, `uname -r`, and `which` to detect the OS and common tools (`bash`, `python3`, `gcc`, `git`, `curl`, `jq`). The result is cached and included in the system context.

### Forbidden Patterns

The security policy’s `forbidden_patterns` is enforced at the tool execution layer. Commands matching any pattern are rejected with a JSON error response. It is strongly recommended to include `"cd"` to prevent the model from changing the working directory.

---

## Runtime Lifecycle & Dependency Injection

`Runtime` is not a singleton. `main()` constructs a single instance and owns every
major collaborator as a `unique_ptr` member:

```
main()
 ├─ pu::Runtime runtime;
 ├─ RunChat(runtime)
 ├─ RunConfig(argc, argv)   // `pu config` — standalone, no Runtime
 └─ catch (std::exception&) → friendly message
```

Key responsibilities:

- `Initialize(config_path)` — loads `agents.json`, creates `AgentManager`, `SessionStore`, `Executor`, `CommandRouter`, then builds the default toolbox.
- `CreateSession(owner, agent, backend)` — creates a session; `backend` is optional.
- `SwitchAgent(agent)` — updates the active agent and rebuilds the toolbox.
- `ProcessInput(...)` — routes either to `CommandRouter` (commands) or to `Executor` (messages).

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
                         ├── Inject system context into chat history
                         ├── Read Workspace.Transcript
                         ├── LLMProvider.Chat()
                         ├── Toolbox.ExecuteTool() → JSON response
                         ├── Extract std/error → store in Transcript
                         ├── Repeat tool loop if tool calls present
                         └── Return final response
                         │
                         ▼
                       SessionStore.SaveSession()
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
│          pu::mcp::StdioTransport             │  ← Child process stdio, line-delimited JSON
└──────────────────────────────────────────────┘
```

MCP servers are configured per-agent via `mcp_servers`. When an agent becomes active, `Runtime::RebuildToolbox` spawns its servers, performs the handshake, lists tools, and registers them with a `mcp.<server>.` prefix.

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
~/.pu/sessions/
├── session_123456.json   # Full session state
└── session_123457.json
```

Each session file contains serialized `Workspace` and `RuntimeSpec`.

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
├── config_cli.hpp        # `pu config` CLI entry (RunConfig)
├── session_store.hpp     # SessionStore
├── cli.hpp, error.hpp, path_utils.hpp
├── core/                 # Logging
├── infra/                # Platform utilities
├── llm/                  # LLMProvider, Ollama/OpenAI providers, streaming parser
├── mcp/                  # McpClient, JsonRpcClient, StdioTransport
├── session/              # Session, Workspace, Transcript, Memory
└── tools/                # Toolbox, built-in tools, MCP adapter, tool_result

src/
├── app/                  # CLI entry (main), UI helpers
├── config_cli.cpp        # `pu config` subcommand dispatch
├── config_tools/         # Agent CRUD, wizard, model scanner, system probe
├── agent_config.cpp, agent_manager.cpp
├── runtime.cpp, command_router.cpp
├── executor.cpp
├── session_store.cpp
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
- **New `pu config` subcommand**: Add handler in `src/config_cli.cpp`, implement the logic in `src/config_tools/`, update `PrintUsage()` and the README command table.
- **External tool (no C++)**: Add an `mcp_servers` entry to `agents.json` — tools are discovered automatically when the agent becomes active.

---

## Known Limitations

- MCP transport supports both POSIX (`fork`/`execvp`) and Windows (`CreateProcess` + pipes).
- MCP request timeout fixed at 5 seconds.
- Multiple `mcp_servers` entries per agent are fully supported; each server is started as a separate client and its tools are registered with the `mcp.<server_name>.` prefix.
- Compaction only supports truncation; `"summarize"` strategy is reserved.
- Environment probing uses `uname` and `which`, which may not be available on all systems (e.g., minimal containers). It gracefully fails and logs a warning.

---

## License

GPL-3.0 — see [LICENSE](../LICENSE)
