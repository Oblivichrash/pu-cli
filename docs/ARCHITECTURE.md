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

---

## Error Handling

### What a command exits with, and what it prints

Every subcommand answers with the same five statuses (`include/pu/app/cli.hpp`), separated by
what a caller should do next rather than by which layer failed:

| Code | Meaning | The caller's next move |
| --- | --- | --- |
| `0` | the command did what it was asked | nothing |
| `1` | the invocation or the configuration is wrong | fix it; a retry cannot help |
| `2` | the provider refused or could not be reached | retry may work |
| `3` | the sandbox refused every tool call the turn made | change the command or the policy |
| `4` | a person stopped it | neither retry nor fix |

A turn the sandbox refused and the model recovered from is `0`: the refusal is advice the
model read, not the turn's outcome. A refusal whose reason is that no channel could ask for
approval is `1`, not `3`, because the command is fine and the configuration is not.

stdout carries what was asked for — the reply, a model listing, a rendered config — and
stderr carries everything else: notices, usage, the reason a turn failed. `pu ask "... | ..."`
pipes the answer and nothing beside it.

### The frame every command runs inside

`cli::WithInitializedRuntime` loads the configuration, runs a body, and saves the session.
Every subcommand that needs a `Runtime` goes through it, so initialization and persistence
are stated once: a failure to initialize is `1`, because the configuration is what is wrong,
and a command that returns without persisting is the exception, not the rule. A non-empty
agent name overrides the configured agent for that run only.

Two signatures exist for a reason rather than for symmetry:

- `RunProbe` reads a product named on the command line without an `agents.json` entry — its
  host and key come from the gateway table, and neither the configuration nor the session is
  touched. That is what makes probing a gateway safe from inside a workspace already
  configured for another one.
- `RunSse` takes a URL, a method and a set of headers from the caller. A feed is not a chat
  turn, so nothing about it is read from `agents.json`; it borrows the runtime only for the
  `HttpClient` and the cancellation token.

`FormatSseEvent` is exposed rather than private because it is what a caller's pipe reads, and
a pipe is testable where a socket is not.


A condition the code can know in advance is checked, not thrown: an operation that can
fail visibly returns `std::optional` or `nullptr`. Exceptions are for what cannot be known
in advance — a body that does not parse, a peer that closes the socket, a provider that
answers with an error frame.

All non-recoverable runtime errors derive from one base class:

```
pu::RuntimeError : std::runtime_error
  ├── pu::RequestRefused        (the state cannot serve a well-formed request)
  └── pu::Error                 (the request or the configuration is wrong)
        └── pu::HttpError       (HttpClient failures)
```

`pu::RequestRefused` sits beside `Error` rather than under it: `pu serve` answers a refusal
with `400` and an `Error` with `500`, so deriving one from the other would report a
caller's mistake as a fault of this process. The status a route chooses, and the body that
carries it, are in [web-api.md](web-api.md).

A catch earns its place only by changing the control flow — returning a value, retrying,
falling back, converting, or ending the process. Logging is not handling, and a catch
whose whole body logs before the path carries on as if nothing happened is the shape to
avoid. The three boundaries where reporting and continuing coexist:

| Boundary | Why a catch is there | What it carries |
|----------|---------------------|-----------------|
| A call site that parses | `json::parse` throwing is how a byte string says it is not JSON | the value the caller wants: a default, a fallback, a marked-invalid record |
| A background thread (`McpClient`, the WebSocket reader) | no caller exists above it to be told | the log is the report; the promise or socket carries the outcome |
| The process boundary (`main`, CLI entry points) | returning is no longer an option | the message is the entire response |

A failure that cannot be converted is raised, not answered with a plausible-looking empty
value: a `tools/list` that fails is not a server with no tools, so `McpClient::ListTools`
throws and `Runtime::RebuildToolbox` skips that server the way it skips one that never
connected.

An absent `agents.json` is an ordinary state, not a failure: `config::FindConfigPath`
returns an empty string and `FindServeOptions` returns `std::nullopt`. Only
`Runtime::Initialize`, which cannot continue, turns the empty path into the `Error` naming
where the file goes. A function reports that it cannot answer rather than picking one
caller's condition to raise.

`Runtime::Initialize` resolves the configuration in one order: an explicit path argument,
then `PU_AGENTS_CONFIG`, then `./.pu/agents.json`, then the user configuration. The
environment variable is checked before the conventional locations so a test or a packaged
run can point at a fixture without a writable workspace, and it is an outright failure
when the file it names is missing — falling back to the conventional path would silently
run against the wrong configuration rather than the one the caller asked for.

The same holds for invariants: `Runtime::ActiveAgent` and `ConfiguredBackend` do not check
that `Initialize` ran, because every path reaching them passes through it. A test for a
condition that cannot be false asserts that the code distrusts itself.

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

## Executor

The tool loop holds no session state (`main()` injects its collaborators) and
runs once per turn: read the `Conversation`, inject system context, call the
provider, execute any tool calls through `Toolbox`, store each structured result
verbatim, and repeat until the reply carries no call.

**Shape of a turn.** `Execute` takes the caller's inputs once and packs them into a
`Turn` — provider, toolbox, callbacks, and the two cancel tokens — so the three steps
below each take that one argument instead of re-threading a seven-parameter signature:

| Step | Does |
| :--- | :--- |
| `CallProvider` | Builds the request path from the `Conversation`, streams the reply, and reports usage. Owns the only `try`/`catch` around a provider call, including the distinction between a cancelled call and a failed one. |
| `RunToolCalls` | Resolves the tool context once, then per call runs the tool, reports it to the callbacks, and stores the result verbatim. |
| `RunToolLoop` | Iterates the two above until a reply carries no call, and is the only place the iteration cap and the "empty reply" failure are decided. |

**Tool context lifetimes.** `ToolContext` holds raw pointers into values that must outlive
the tool call, so the `Control` and `Approval` structs it points at are `Executor` members
filled in by `BuildToolContext`, not locals. `Control` is present only when the caller
supplied a stop token — the CLI has no channel to stop a command, so its pointer stays
null there.

**System context.** `Executor` prepends a system message built from:

- OS name and kernel version (probed once, see below)
- Security policy (sandbox root, forbidden patterns)
- Current working directory (the sandbox root)
- Tool-use guidelines for the model
- Whatever `context_digest` returns, when a caller set one

It is merged with the agent's configured `system_prompt`, which comes from
`agents.json` via `Runtime::RebuildToolbox` and is not session state, so
switching the backend does not clear it.

**Structured tool output.** Tools return JSON through the schema in
`include/pu/tools/tool.hpp`: `success`, `blocked`, `output`, `error`, `exit_code`, and the
`refusal` that names why the policy refused a call when one did. The header is the field
set's one home — `tools::Refusal` beside it says which refusal a caller can retry and which
is a configuration to fix. The executor stores the envelope verbatim so the model sees what
the tool produced, and reads `output`/`error` out of it only for the tool callbacks.
`Toolbox::ExecuteTool` answers with the same envelope when the named tool is absent, so
callers never parse two shapes.

**Environment probing.** `Executor::ProbeStaticEnvironment()` runs once during
construction (`uname` on POSIX, the Windows kernel API elsewhere) and caches the
result. No tool-binary detection is performed.

**Paged tool output.** `execute_bash` answers with one page of what the command
printed. That shape is four nouns and a budget, in `include/pu/paging/paging.hpp`:

| Noun | What it is |
|---|---|
| `Buffer` | one run's output, indexed by line, immutable once stored |
| `Page` | a contiguous run of a buffer's lines, with the cursors either side |
| `Cursor` | `<buffer>:<line>` — where a page starts |
| `Pager` | the buffers a session can still read, and the `Budget` that decides |
| `Anchor` | where a read starts: `head`, `tail`, or a cursor |

`core::Run` (`include/pu/core/process.hpp`) produces the other half: the text, an
`Extent` saying how much of the stream survived, and the verdict. Output and verdict are
one value because only the reader of the stream knows both — a reader that looked away
has to say the command was stopped, and an exit code means nothing without that. The
budget lives in the `Pager` alone; the tool reads it from there, so a page size and a
retention policy cannot disagree.

A cursor identifies its page by itself: it names the run, and the run knows its command,
so paging does not restate the command. The store belongs to the runtime and is borrowed
by the tool at construction, because `RebuildToolbox()` replaces every tool object on a
model or agent switch and a cursor has to outlive that. What a page cannot reach is
reported rather than hidden: `page` says where the page sits, `extent` says how much of
the run survived, and `buffer` says which run it was.

The page's wire form is paging's too, in both directions: `Envelope` writes the three
scopes, `OriginOf` reads a receipt back, `SupersededReceipt` empties one. A writer in the
tool and a reader in the projection is how the two drift apart, and a receipt is only ever
read by machinery that did not write it. The origin is read once, where the result enters
the conversation (`ToolPayload::origin`), and stored with the node: a page's provenance is
a fact about the text, and re-deriving it from the text on every turn is the same question
asked for as long as the conversation lasts.

**One page per source.** A buffer carries two ids and an order: `id` names the run,
`source` names the question — the command and the directory — so two runs of one command
share a source, and `rank` says how late the run was. Both ids are computed from what
produced the run rather than counted from one, because a counter starts over with the
process: the source would make a resumed transcript read the new `s1` as the old `s1`, and
the run id would make this process's first run the same run as the last one's. The run id
carries the mark of the pager that wrote it, so a cursor written elsewhere — by an earlier
process — finds nothing instead of a namesake. `BuildRequestPath` compares the
`rank` a receipt carries: while it assembles a request it replaces the page of a run that
the same source has since been run again to supersede, leaving the verdict, the `page` and
the `extent` in place and naming the newer run in `superseded_by`. A page is only superseded by a newer run
read *after* it, so a run's own pages stay (they are complementary, not successive) and so
does a page the model paged back to. A pager opening a transcript continues the numbering
past the highest rank it finds there (`ContinueRankingFrom`), so a run made after a restart
is not ranked below every page the transcript already holds. The substitution lives in the
projection, not in the graph, so a rewind or a reload still sees every page that was ever
read, and the buffer it replaces is still stored — the receipt is a pointer, not a dead
end.

**Forbidden patterns.** `forbidden_patterns` is enforced at the tool execution
layer: a matching command is rejected with a JSON error. The policy fields live in
[configuration.md](configuration.md#agentsjson).

---

## Runtime

`Runtime` is not a singleton: `main()` constructs one and owns every major collaborator as
a member, and `pu serve` runs that same instance behind the Beast front-end. Everything
harder than "it exists" — the lifecycle, dependency injection, cancellation contract, the
HTTP handler rules, static serving, single-session persistence, tool and MCP shutdown —
is in **[runtime.md](runtime.md)**. This document keeps to what the tree is made of and how
a change reaches it.

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

Chat runs exclusively over the `/ws` WebSocket. A run message is queued to the
`SessionRunner`, whose single worker starts `ProcessInput`; its `content_callback`
hands each streamed chunk back through the connection, which queues it and writes it on
the connection's strand, producing the typewriter effect in the browser. The
`ToolCallbacks` passed alongside it emit tool start/end events keyed by the tool call
`id`. Commands and non-streaming backends deliver their full text as a single chunk
frame. The frame schema is owned by [web-api.md](web-api.md) and implemented by
`web/ws.js`.

The read loop is what an asynchronous stream buys: a connection parked in a synchronous
read cannot be woken by closing its socket, so a shutdown would wait on a client that
will never speak again. Here the run happens on the runner's own thread and the stream
belongs to the strand, so shutdown stops the `io_context` and every read returns at once.

A turn belongs to the client watching it. Closing the page or losing the socket ends it,
and nothing of it is kept: what it had written is half an answer, and storing that would
make the next request read it as the model's finished reply. The chat is therefore never
driven by nobody, and a page that reloads mid-answer comes back to the conversation as the
store has it — the question, and no reply to it. A `cancel` the page asked for is the one
case that still answers: the turn stops, keeps what already arrived, and ends with `done`
so the page can stop waiting. A socket the same page replaces — a reload, or the watchdog
giving up on a socket that went silent — is not the page leaving, so the turn runs on and
the frames produced while the swap happens are held for the socket that attaches next.

### Browser modules

The page is plain scripts with no build step, so the load order in `web/index.html` *is*
the dependency order. The modules, their ownership and the static page resolution have
their own document.

See [web-ui.md](web-ui.md).

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
./.pu/logs/                    # Log output, moved by PU_HOME
```

`<workspace>` is the directory `pu` was started in. Logs are the only thing
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
`/backend <type> <model>` adopts an agent named after the type, so the backend a
session reports and the agent it names are always the same answer. It takes no key:
a key typed at the prompt is written into the scrollback, so an endpoint that needs
one is named in `agents.json`, where it can come from the environment. The session
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

## Directory Structure

The tree is layered bottom-up: `core/` holds dependency-free base utilities,
the domain modules sit beside it, and the orchestration headers/modules live at
the root of `include/pu/` and `src/`.

```
include/pu/                  src/
├── app/                     ├── app/                  # main, CLI parsing
├── config/                  ├── config/               # agents.json, the backend model, the gateways
├── core/                    ├── core/                 # logging, platform, HTTP client, running a command
├── llm/                     ├── llm/                  # protocols, providers, streaming parser
├── mcp/                     ├── mcp/                  # transports, JSON-RPC client
├── paging/                  ├── paging/               # the buffers a session can still read
├── runtime/                 ├── runtime/              # runtime, executor, command router
│   ├── command_router.hpp   ├── serve/                # `pu serve`: listener, REST routes, WebSocket
│   ├── executor.hpp         ├── session/              # Session, Conversation, message graph
│   └── runtime.hpp          └── tools/                # Toolbox, tools
├── session/
└── tools/
```

A header lives in its module's directory; `include/pu/runtime/` holds the
orchestration layer (`Runtime`, `Executor`, `CommandRouter`). A header whose
module has no directory of its own stays beside the sources that include it.

The build is a chain of static libraries, one per layer, each linking only the ones below it:
`pu_core_util` (logging, platform, process, SSE, the HTTP client) is the bottom, `pu_paging`
beside it, then `pu_llm`, `pu_model`, `pu_config`, `pu_session`, `pu_mcp`, `pu_tools`,
`pu_runtime` and `pu_serve`, with `pu_core` on top holding only the command layer. The
`pu` executable links `pu_core`. The split is what makes the layering a fact the linker
checks rather than a habit the includes happen to keep: a module that reaches upwards fails
to link, and the failure names the symbol.

One link goes the other way on purpose, and only one. `pu_tools` calls `RunDelegatedTurn`,
declared in `tools/delegation.hpp` and defined in `runtime.cpp`, so `pu_tools` links a weak
fallback that answers "no turn can run here" and `pu_runtime` links the definition that
replaces it. A tool states a question; the layer that can answer one supplies the answer.
Without that seam `ask_dir` would have to construct a `Runtime`, which is the one upward
dependency the tools would otherwise carry.

`web/` is the page the server mounts, served straight from the directory the server resolved,
so adding a module means adding the file and its `<script>` tag and nothing else. A target that
names a directory, or is not there, is a `404` rather than a read of the directory. Its module
split is described in [web-ui.md](web-ui.md).

### Gates, protocols and providers

Three facts about a vendor that look alike are three different things: a **protocol** is
*how to talk*, a **gateway** is *which product*, and a connection is *where*. `model/gateway.hpp`
holds the middle one: `GatewayId` names the gateways a build carries, `Gateway` is
`{id, name, default_host, api_key_env, protocol, endpoint_path, make_provider, make_listing,
listing_headers, billing}`, and `Gateways()` / `FindGateway()`
/ `GatewayForId()` answer from one explicit table. Every gateway
lives in `src/llm/gateway_table.cpp` with its neighbours: a gateway is mostly a table row plus
the mapping from the backend config to its protocol's config, and those mappings are only
readable as a set — side by side, a field one gateway fills and another drops is visible, and
in one file per gateway it is not. `MakeOpenAIProvider` and `MakeOllamaProvider` share
`FillProtocolConfig`, which is why the Ollama mapping cannot drift from the OpenAI one.

The table is written where the factories it names are visible — above `model`, in `llm` — and
the reader below lives in `model`. A program therefore installs the table once, from its own
entry point, with `model::InstallGatewayTable()`: `main.cpp` and the test runner both do, and
a lookup made before that falls back to the one `llm` wrote, so the install is an
optimisation of the direction rather than a trap.

Billing follows the same split. What a call costs is read in the protocol layer, through
`ResponseShape`: one gateway names the field `credit`, another names nothing at all, and the
difference never leaves `TokenUsage::credit`. The upper layers never branch on which gateway
is in use — there is no `if (cfg.gateway == kCodeBuddy)` in the code above the protocol, and
a turn's usage is the sum over every round its tool loop ran, accumulated in the executor.

`agents.json` names a gateway through `gateway`, and a missing or unknown name is refused rather
than defaulted, so a backend never reaches a host its author did not choose. The field is what
a stored config carries; `BackendTypeName()` / `ParseBackendType()` are gone, and one parser
resolves the name.

A protocol stays what it was — the implementation of how a gateway talks. `openai` is split into
`llm/openai_protocol.{hpp,cpp}` (the wire format) and `OpenAIProvider` (that protocol at one
host with one set of headers), because a change to the SSE parse and a change to a gateway's
headers are different changes; `ollama` keeps protocol and provider in one class. The gateway
abstraction stops short of one thing the design chose not to build yet: a gateway offering more
than one protocol, which would need an `entry` between the gateway and the protocol. Every gateway
measured so far speaks exactly one, so that layer is deferred rather than carried as unused
state.

### Dependency direction

Includes point one way: `core/` is a leaf, `config/` and `llm/` build on it, and
`mcp/`, `tools/`, `session/`, and the orchestration headers build on those. No
directory includes one that sits above it, and `config/` in particular never
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

- **New gateway**: Add a `Make<Name>Provider`, a `Make<Name>Listing`, a `<Name>Gateway()` and a table row in `src/llm/gateway_table.cpp`, stating the name, the default host, how to build the gateway's provider, and how to build its catalogue read; then add the pointer to `kGateways`. A gateway that speaks an existing protocol reuses its provider and inherits `StreamingProvider`; one that does not supplies its own. Removing a gateway is the same in reverse - the four fragments and its line.
- **New tool**: Inherit `pu::Tool`, implement methods, register in `Runtime::RegisterBuiltinTools()`.
- **New command**: Add handler in `CommandRouter`, route, update help.
- **External tool (no C++)**: Add an `mcp_servers` entry to `agents.json` — tools are discovered automatically when the agent becomes active.

---

## Known Limitations

- MCP requests time out after a fixed 5 seconds.
- Environment probing can come up empty: an absent `uname` on POSIX yields nothing, and a failed Windows kernel API falls back to `"unknown"`.
- **The tool-call budget is the turn loop's one automatic stop.** It counts calls rather than
  replies, since one reply can carry several, and it is checked between rounds: a budget of 2
  lets two calls run and then appends an instruction to wrap up, so the model answers from what
  it has instead of the turn failing. A wrap-up round that calls a tool anyway ends the turn with
  `TurnOutcome::kToolBudgetReached` and an error naming the setting, rather than looping. The
  number comes from the agent's `tool_call_budget`, or from the session's override when the page
  or `/api/settings` set one.
- **A session's cost is summed, not stored.** `UsageLedger::Total()` adds the counts of every
  turn the conversation still holds. A price is summed only while every turn that reported one
  used the same unit, and only if every turn was priced: one unpriced turn or two units make the
  total carry no price at all, because a number that covers part of the session reads as the
  whole of it. `priced_turns` says how many were priced, so a page can explain the absence.
- **Nothing enforces a token budget, by design.** `ChatResult::usage` carries what the provider counted, and the executor logs it at `debug`, but no limit is compared against it, so a conversation still grows until the provider refuses it and the refusal reaches the user as an HTTP error. Tool output is the one thing that is bounded up front: `execute_bash` returns a page and leaves the rest in the pager.
- **A cursor does not outlive its buffer.** The pager keeps the last 8 buffers within 4 MiB; a page for one already dropped is refused rather than answered from another command's output, so the model runs the command again. A cursor names its own run, so the other half of that risk does not exist: a page request cannot reach a different command's output and does not have to restate the command to prove it.
- **On Windows a silent command cannot be stopped.** `exec::Run`'s Windows reader is a blocking `popen` read and notices the stop token only between two reads, so a command that prints nothing runs to its own end there. The POSIX reader waits in `poll` and kills the process group, which stops the same command on Linux and macOS.
- **A `max_tokens` cap that lands inside a tool call ends the turn with an error.** The reply stops mid-argument, so the call never completed and its text is not a usable argument; the provider raises rather than keeping the fragment. A `finish_reason` other than `length` leaves a non-JSON argument as it stands.
- **A cancelled run keeps no partial reply, by design.** The transport aborts the stream and the executor ends the turn with neither a reply nor an error, so nothing is appended: the session holds the user message and no answer, and a follow-up "continue" restarts the answer rather than resuming it.
- **The store is only persisted after a completed interaction and on shutdown.** A crash loses everything since the last save, and the store is held in memory in between.
- **A stopped MCP server is killed rather than negotiated with.** The child gets `SIGTERM`, two seconds of polls, then `SIGKILL`; a server that needs longer to flush on the way out does not get it.
- **A delegated turn cannot be asked for approval, and cannot be sandboxed tighter than
  its own file says.** The child `Runtime` never calls `EnableApprovalGate()`, so a tool
  call its directory's `approval_patterns` cover comes back `kNobodyToAsk` even when a
  user is sitting in front of the page: the approval channel belongs to the parent's
  toolbox, not the child's. And the parent's sandbox does not reach into the child at all
  — `Inside()` guards the `ask` string, not the work that follows it, so a directory you
  delegate to is trusted as far as its own `agents.json` allows. Both are deliberate: a
  child that inherited the parent's policy would turn that file into a suggestion instead
  of the boundary.
- **A stopped or overlong delegated turn is reported, not resumed.** Its answer is
  dropped, and the asking model reads a note saying so; the work the child did is still in
  the child's own turn, which the parent never sees.
- **The serve registry names a server by its pid, not by a claim.** A server that is killed
  rather than stopped leaves its line behind, and a pid the operating system later hands to an
  unrelated process reads as a live server until that process exits, so the page can list a
  workspace nothing is serving. Nothing acts on a record beyond drawing it: a wrong row costs a
  wrong name, not a message sent to the wrong place. Two smaller limits come with it: a
  machine with no `HOME` gets no list, since there is nowhere machine-wide to write it and
  every workspace's file is the wrong place; and a lock that is never released costs the few
  milliseconds the writer waits before it writes anyway rather than costs the server.
- **The transcript fold is checked by running the page's own JavaScript only where `node` exists.** Without it the same case file runs against a second implementation in Python, which catches a case that has drifted but not a page that no longer runs.

