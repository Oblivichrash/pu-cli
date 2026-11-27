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
| `ConnectMcpServers` / `DisconnectMcpServers` | Free functions that connect a server set and disconnect it again — the whole connect/disconnect sequence, so `Runtime` holds the result rather than driving the loop |
| `JsonRpcClient` | JSON-RPC 2.0 protocol layer |
| `Transport` | Abstract MCP transport (`Start` / `Stop` / `WriteLine`) |
| `StdioTransport` | stdio subprocess transport |
| `HttpTransport` | remote streamable-HTTP transport (BeastHttpClient POST, line-delimited responses) |

What a command exits with, what it prints, and which exceptions exist are in
[errors.md](errors.md): the five statuses are a contract with a script, not a property of
the tree.

---

## JSON Handling

All JSON parsing and serialization is provided by **Boost.JSON**
(`boost::json::value`). `include/pu/core/json.hpp` is a thin convenience layer over
it: `parse`, `serialize`, `ValueOrDefault`, `HasKey` and `PrettyPrint`. Each one
carries its own contract where it is declared, and the helper is what to reach for
rather than the raw Boost call, so the fallbacks stay in one place.

Which one to reach for, when a member may be absent or of the wrong type:

| Member | Answer for a member that is absent, null, or of another type |
|---|---|
| `HasKey(v, "k")` | `false` only for an absent name or a value that is not an object; a name holding `null` is still a key, so `true` |
| `ValueOrDefault<T>(v, "k", fallback)` | `fallback` — for a value the caller can proceed without |
| `OptionalOrDefault<T>(v, "k")` | `std::nullopt` — for a value whose absence means something, so a caller can tell "not sent" from "sent as zero" |

`HasKey` is the right question only when the caller does its own type check; reach for it
before `at()`, not instead of `ValueOrDefault`. Never read a member through `at()` without
one of the three: an absent name throws, and a throw on a gateway field is a stream that
stops instead of a default being taken. The rule applies to every JSON surface named below,
so a config file missing a field degrades the same way a stream frame does.

JSON is used for configuration (`agents.json`), session persistence
(`Session::Serialize` / `Session::Deserialize`), structured tool output
(`pu::tools::tool.hpp`), the MCP JSON-RPC layer, and the WebSocket/REST
API in `src/serve/serve_http_routes.cpp` and `src/serve/serve_websocket.cpp`.

---

## Runtime

`Runtime` is not a singleton: `main()` constructs one and owns every major collaborator as
a member, and `pu serve` runs that same instance behind the Beast front-end. Everything
harder than "it exists" — the lifecycle, dependency injection, cancellation contract, the
HTTP handler rules, static serving, single-session persistence, tool and MCP shutdown —
is in **[runtime.md](runtime.md)**. This document keeps to what the tree is made of and how
a change reaches it.

## Where the rest of it lives

The sections this document used to carry have their own homes, each because a reader
arrives at them from a different direction than "what is the tree made of":

| Was here | Now in | Arrived at by |
|---|---|---|
| Error handling, exit codes, the exception hierarchy | [errors.md](errors.md) | a script that has to branch on an exit status |
| The tool loop, paging tiers, `BuildRequestPath` | [runtime.md](runtime.md) | a maintainer tracing one turn |
| Cancellation and streaming | [runtime.md](runtime.md) | the same reader, one section later |
| Directory tree, CMake targets, dependency direction | [layout.md](layout.md) | a contributor adding a module |
| Known limitations | [limitations.md](limitations.md) | someone with a symptom, asking whether it is by design |

---

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
feature belongs to `MessageNode` (`include/pu/session/message.hpp`), which owns what
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
<workspace>/.pu/logs/          # Log output, moved by PU_HOME
```

`<workspace>` is the directory the run is about: the one `-C` names, and the
directory `pu` was started in when it names none. Logs are the only thing
`PU_HOME` moves, and their directory is per process so two processes in one
workspace never rotate the same file; the environment table in
[configuration.md](configuration.md#environment-variables) owns both paths.

The session file carries `schema_version` beside `conversation` and
`session_spec`. It is written automatically after every interaction and on
shutdown, and restored on startup. A file stamped with a version this build does
not accept is refused rather than read best-effort.

**What each turn cost is kept beside the conversation, not inside it.** `usage` is a
sibling of `conversation` in the file, keyed by the assistant node the turn produced: prompt,
completion and cached tokens, the credit the gateway priced it at, how many provider calls the
turn took, and which model and gateway answered. A model never reads it, `History()` never
renders it, and a page draws it next to the reply rather than as part of it — which is why it
is not a field of `AssistantPayload`. It is kept because it is the only record that an
optimization changed anything: a prompt edited so the cache stops hitting, or a model swapped
for one that costs three times as much, shows up here and nowhere else.

The key is absent from every file written before it existed, and absent is read as "nothing
was recorded" rather than as a version this build cannot load, so the schema version did not
move. A turn that has left the chain — a rewound turn, `/clear` — takes its entry with it on
the next save.

`session_spec` names the agent and carries the session's own thinking level when
`/thinking` set one. Backend fields are read from `agents.json` on each start, so
editing the configuration takes effect without touching the session.
`/backend <type> <model>` adopts the model named by the file, so the backend a
session reports and the agent it names are always the same answer. It takes no key:
a key typed at the prompt is written into the scrollback, so an endpoint that needs
one is named in `agents.json`, where it can come from the environment. A restart
resumes the agent the session names; a name the file no longer carries is reported
and the file's own agent is used, since a file describes exactly one.

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

A tool call has three states: `pending` while it has not been answered, `completed`
once a receipt answers it, and `interrupted` when the turn that requested it was
cancelled before the call ran. Cancelling seals the leaf: every call still `pending`
becomes `interrupted` and gains a receipt recording that it never ran. Those receipts
are chain nodes like any other, so the next request renders them to the model as
`role: "tool"` messages. Only a `pending` call counts as unfinished, so a sealed turn
stops blocking `/rewind` and a switch of agent while a call that may still be running
goes on blocking both. Whether a receipt records a result or an interruption, its
presence is what keeps each tool call paired with an answer, which is what the request
path is rebuilt from.

Three vocabularies name these states, and they are not interchangeable. Storage and the
session file use `pending` / `completed` / `interrupted`
([`message.hpp`](../include/pu/session/message.hpp), [`graph.cpp`](../src/session/graph.cpp)).
`/api/history` renames `completed` to `done` for the Web UI, so a client sees `pending` /
`done` / `interrupted`. The transcript additionally shows `running`, which is live-only
state and is never written to a session.

A file without the version, with another one, or whose history is not node storage is
refused rather than guessed at: `pu` reports the reason and starts a fresh
conversation, and the next save replaces the file.

The session file contains no system prompt: the prompt is configuration and is
read from `agents.json` on every start.

---

## Extension Points

How a change reaches this tree: where each kind of addition lands, and what it has to be
registered with so it is reachable rather than merely compiled.

- **New gateway**: Add a `Make<Name>Provider`, a `Make<Name>Listing`, a `<Name>Gateway()` and a table row in `src/llm/gateway_table.cpp`, stating the name, the default host, how to build the gateway's provider, and how to build its catalogue read; then add the pointer to `kGateways`. A gateway that speaks an existing protocol reuses its provider and inherits `StreamingProvider`; one that does not supplies its own. Removing a gateway is the same in reverse - the four fragments and its line. [layout.md](layout.md#gates-protocols-and-providers) says why the row lives there.
- **New tool**: Inherit `pu::Tool`, implement methods, register in `Runtime::RegisterBuiltinTools()`.
- **New command**: Add handler in `CommandRouter`, route, update help.
- **External tool (no C++)**: Add an `mcp_servers` entry to `agents.json` — tools are discovered automatically when the agent becomes active.
- **New module**: A CMake target at the layer it belongs to, linking only downward. [layout.md](layout.md) has the target list and the direction rule.

---
