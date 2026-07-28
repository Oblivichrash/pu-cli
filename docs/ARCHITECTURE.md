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
| `Toolbox` | Tool registry; executes built-in and Python tools |
| `CommandRouter` | Routes `/` commands to handlers |
| `SessionStore` | Persists sessions to `~/.pu/sessions/` |
| `ForkMergeService` | Branch operations: fork, merge, prune |
| `CallStack` | Delegation stack for nested tasks |

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
├── tools/          # Toolbox, tools
├── storage/        # SessionStore
└── core/           # ForkMergeService, SummaryGenerator, FactExtractor

src/
├── app/            # CLI entry, UI helpers
├── runtime/        # Runtime, CommandRouter implementations
├── session/        # Session, Workspace, etc. implementations
├── executor/       # Executor implementation
├── llm/            # Providers
├── tools/          # Toolbox, tools
├── storage/        # SessionStore
└── core/           # ForkMergeService, etc.
```

---

## Extension Points

- **New backend**: Implement `LLMProvider` and register in `Session::CreateProvider()`.
- **New tool**: Inherit `pu::Tool`, implement methods, register in `Runtime::Initialize()`.
- **New command**: Add handler in `CommandRouter`, route, update help.

---

## Known Limitations

- `--agent` startup parameter does not auto-switch backend (use `/backend` after start).
- `[INFO] Connected to agent:` display is currently empty.
- `/reload-tools` is a placeholder.

---

## License

GPL-3.0 — see [LICENSE](../LICENSE)
