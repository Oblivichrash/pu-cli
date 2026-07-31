# pu-cli Architecture

> "朴散则为器"——《老子》

## Overview

pu-cli is built around four principles:

1. **Session isolation** — Each session has its own workspace, history, and branch tree.
2. **Git-style branching** — Fork/merge isolated contexts with squash or full history.
3. **Dynamic backend switching** — Switch LLM providers without losing state.
4. **Stateless execution** — Executor holds no state; all state lives in Session.

---

## Core Components

| Component | Responsibility |
|-----------|----------------|
| `Runtime` | Singleton managing all sessions, routing commands/messages |
| `Session` | Aggregate root: `Workspace` + `RuntimeSpec` + `CallStack` |
| `Workspace` | State container: `Transcript` (history) + `Memory` (vars) + `RevisionGraph` (branches) |
| `Executor` | Stateless tool loop; reads/writes `Workspace` |
| `LLMProvider` | Model gateway; handles transport + format adaptation |
| `Toolbox` | Tool registry; executes built-in, Python, and MCP tools |
| `CommandRouter` | Routes `/` commands to handlers |
| `SessionStore` | Persists sessions to `~/.pu/sessions/` |
| `ForkMergeService` | Branch operations: fork, merge, prune |
| `CallStack` | Delegation stack for nested tasks |
| `McpClient` | High-level MCP client: handshake, `ListTools`, `CallTool` |
| `JsonRpcClient` | JSON-RPC 2.0 protocol layer; request/response with promise mapping |
| `StdioTransport` | stdio subprocess transport; fork+exec, pipe-based line I/O |

---

## Key Data Types

| Old | New |
|-----|-----|
| `Context` | `Workspace` |
| `Delegation` | `Assignment` |
| `Fact` | `Artifact` |
| `SummaryReport` | `HandoffReceipt` |
| `DelegationStack` | `CallStack` |

---

## Data Flow

```
User Input
    │
    ▼
Runtime.ProcessInput()
    │
    ├── Is command? ──► CommandRouter ──► Session mutation
    │
    └── Is message? ──► Session.CreateProvider()
                         │
                         ▼
                       Executor.Execute()
                         │
                         ├── Read Workspace.Transcript
                         ├── LLMProvider.Chat()
                         ├── Toolbox.ExecuteTool()
                         ├── Write to Workspace.Transcript
                         └── Return response
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

MCP servers are configured per-agent in `agents.json` via `mcp_servers`. At startup, `Runtime` spawns each server, performs the MCP handshake, lists tools, and registers them with a `mcp.` prefix in the `Toolbox`. No external MCP SDK is required — the client is fully self-contained.

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
├── agent/          # AgentManager (config metadata only)
├── runtime/        # Runtime, CommandRouter
├── session/        # Session, Workspace, Transcript, Memory, RevisionGraph, CallStack
├── executor/       # Executor
├── llm/            # LLMProvider, providers (Ollama, OpenAI)
├── mcp/            # McpClient, JsonRpcClient, StdioTransport, types
├── tools/          # Toolbox, tools (including McpTool adapter)
├── storage/        # SessionStore
└── core/           # ForkMergeService, SummaryGenerator, FactExtractor

src/
├── app/            # CLI entry, UI helpers
├── runtime/        # Runtime, CommandRouter implementations
├── session/        # Session, Workspace, etc. implementations
├── executor/       # Executor implementation
├── llm/            # Providers
├── mcp/            # MCP transport, JSON-RPC, client implementations
├── tools/          # Toolbox, tools
├── storage/        # SessionStore
└── core/           # ForkMergeService, etc.
```

---

## Extension Points

- **New backend**: Implement `LLMProvider` and register in `Session::CreateProvider()`.
- **New tool**: Inherit `pu::Tool`, implement methods, register in `Runtime::Initialize()`.
- **New command**: Add handler in `CommandRouter`, route, update help.
- **External tool (no C++)**: Add an `mcp_servers` entry to `agents.json` — tools are discovered automatically.

---

## Known Limitations

- `--agent` startup parameter does not auto-switch backend (use `/backend` after start).
- `[INFO] Connected to agent:` display is currently empty.
- `/reload-tools` is a placeholder.
- MCP transport is POSIX-only (`fork`/`execvp`); Windows support not yet implemented.
- MCP request timeout is fixed at 5 seconds.

---

## License

GPL-3.0 — see [LICENSE](../LICENSE)