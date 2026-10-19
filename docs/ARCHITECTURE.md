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

JSON is used for configuration (`agents.json`), session persistence
(`Session::Serialize` / `Session::Deserialize`), structured tool output
(`pu::tools::tool.hpp`), the MCP JSON-RPC layer, and the WebSocket/REST
API in `src/app/serve_http_routes.cpp` and `src/app/serve.cpp`.

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

**Forbidden patterns.** `forbidden_patterns` is enforced at the tool execution
layer: a matching command is rejected with a JSON error. The policy fields live in
[configuration.md](configuration.md#agentsjson).

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
2. An Asio `io_context` drives the `tcp::acceptor` on its own thread, with
   `reuse_address` set before the bind so a restart can take a port a just-closed
   server left connections in `TIME_WAIT` on.
3. Each accepted connection is dispatched on a strand of its own. A plain HTTP request
   is read, dispatched to the static/REST routes, and answered on that strand, and the
   connection ends with the response. Both the read and the write are asynchronous: a
   socket parked in a blocking read would hold the one thread the whole server runs on,
   so a browser's idle preconnect would stall every other page until it gave up. The
   request line and headers get a 30s deadline, and a connection that sends none is
   dropped. A WebSocket upgrade on `/ws` is handed to a `WebSocketConnection`, whose I/O
   is asynchronous from there on.
4. A second upgrade while a page is attached is answered with a `busy` frame and
   closed: one session, one page. Claiming the session happens when the upgrade is
   accepted, not when the first run arrives, so the refusal reaches the peer before its
   socket goes. The page names itself with `?client=` on the upgrade, and the claim is
   compared by that name: the same tab reconnecting takes the session back and the
   socket it left behind is closed from its own strand, while another tab is refused.
   Without a name, every connection is a page of its own.
5. A run is queued to a single `SessionRunner` worker, which runs `ProcessInput` and
   posts each frame back through the connection. The runner holds only the token that
   steers the turn; the worker reads it by value, so a `cancel` on the strand reaches a
   turn in flight without waiting for it. The message schema, the REST route table, and
   the status each refusal carries are in [web-api.md](web-api.md).
6. On Ctrl+C the acceptor is closed, the `io_context` is stopped — which aborts every
   pending read and write, so no connection thread is left blocked — the runner is
   stopped, and `Runtime::Shutdown()` persists the session.

### Cancellation contract

A turn is steered by tokens, never by a process-wide flag, so two turns in one process
cannot stop each other.

| Channel | Scope | Effect |
| :------ | :---- | :----- |
| `cancel` | the turn, a running command included | The turn ends, the reply keeps what already arrived, and every tool call it never ran is sealed as interrupted |
| shutdown | the process | Every turn in flight is abandoned, and its partial reply is not persisted |

A command is not stopped on its own: the composer has one button, and it ends the turn the
command belongs to. A sealed call appends a `tool` node carrying `kCancelledReceipt`, which
tells the model the turn is over and that it should wait for the user rather than resume on
its own. A command killed by the turn's own token still reports `kCommandStoppedReceipt`,
which tells the model the process died rather than answered.

The sealed receipts belong to the turn's chain, so the next request carries them. They are
appended after the interrupted assistant node and become the leaf, which is what puts them
in `Chain()`. This does not block a rewind — `HasUnfinishedToolCalls()` counts only
`kPending`, and a sealed call is `kInterrupted`.

`platform::ShutdownRequested()` answers "has this process been asked to exit", and it is
the one signal that is **not** a turn's cancel token. It is set by the signal handler and
by the Web UI's stop, and it is never cleared while the process lives — a process on its
way out does not start another turn. `platform::ShutdownToken()` exposes the same fact as
a `CancelToken` for the call sites that take one, and the two cannot disagree because the
token is the storage and the accessor only reads it.

Treating shutdown as a cancel is what a caller must not do: a cancelled turn keeps its
partial reply and seals its unrun tool calls, because the session goes on; a shutdown
persists neither, because it does not.

### HTTP handler contract

`DispatchHttpRequest` (`src/app/serve_http_routes.cpp`) does all the work a handler
should not have to: it parses the body once for routes that take one, takes the `io_mutex`
for the duration of the call, wraps whatever the handler returned in the envelope that
route declares, and turns a thrown exception into the status that names whose fault it is
(`RequestRefused` → `400`, `Error` → `500`).

A handler therefore only computes and returns:

```cpp
boost::json::value HandleApiRewind(Runtime& runtime, const boost::json::value& body);
```

Refusals are thrown rather than sent, and each route declares its own success shape —
`{"success": true, …}` merged into the handler's object, a bare array for `/api/history`,
or a named collection for `/api/agents` and `/api/workspaces` — so the wire format is
read off the route table instead of being reassembled by hand in every handler.

The status the exception maps to is the one that names whose fault it is: `RequestRefused`
for a request the caller can fix, `NotFound` for a name that does not exist, and `Error`
for an operation that failed on our side.

### Static page serving

The UI travels with the binary, not with the workspace the session is pointed at, so
`GetWebDir` resolves the page directory in this order and takes the first that exists:

| Order | Candidate | Why |
| :---- | :-------- | :-- |
| 1 | `$PU_WEB_DIR` | The one explicit override, for a checkout whose layout we cannot guess |
| 2 | `<executable>/web`, `<executable>/../web`, `<executable>/../share/pu/web` | A build tree or an install keeps the page under its own prefix, and a workspace somewhere else must not hide it |
| 3 | `./web`, `../share/pu/web`, `/usr/share/pu/web`, `/usr/local/share/pu/web` | Release and system installs, where the page sits beside the working directory |

Only the files in `kPageFiles` are served. A target outside that list is answered with
the `404` that ends `DispatchHttpRequest`, so the route cannot be walked to read anything
else in the page directory.

The target is joined to the page directory with `operator/`, after the leading slash is
dropped and the bare `/` is mapped to `index.html`. Concatenating the strings instead
would make the result depend on whether the configured directory happens to end in a
separator, and would misreport a correctly configured but separator-less directory as a
missing page.

On macOS the executable's own path is reported by `_NSGetExecutablePath`, which fills a
buffer whose size it also reports and may leave `..` in the result, so it is resolved
before its directory is taken.

### Single-session auto-persistence

`Runtime` maintains a single `std::shared_ptr<Session> current_session_`. On
`Initialize()` it loads `<workspace>/.pu/session.json` (if the file exists) via
`Session::Deserialize`. After every `ProcessInput()` call and on `Shutdown()`, the
session is serialized back to the same file via `Session::Serialize`. There is no
manual save/load/list/export; persistence is fully automatic and scoped to the
data directory.

### Tool execution

A tool runs as a child process and answers with one merged stream. Its stdout and stderr are
the same pipe, so the caller cannot tell them apart afterward and the result carries a single
output field rather than two that would sometimes be empty for reasons the tool does not
control.

The child is put in its own process group. A stop then kills the group, which takes the shell
and any grandchildren with it — killing the immediate pid alone would leave a `bash -c` shell's
children running with the pipe still open, and the read that is supposed to see the token would
keep waiting on them.

The stream is read through `fdopen` on the pipe rather than `popen`, so `pclose` is not
available: it answers for `popen`'s own pid table, and on a stream it did not open returns
success regardless of what the child did. `fclose` closes the descriptor and `waitpid` reaps
the child explicitly, which is the only way the exit status is known.

Reading with `fgets` would park on the pipe until the child writes, so a command that stays
quiet would keep a stop token unchecked until it finished on its own. The read instead runs on
a poll tick: output when there is any, the token when there is not. Windows keeps the `popen`
path and honors a stop token only between reads of the merged stream, which is no worse than
what the stream reader does with a cancel.

### Stopping an MCP server

A stdio server is a child process with three pipes, and stopping it is where a client most
easily hangs itself. Closing the descriptor a reader is blocked on does **not** wake that
read: the close succeeds, and the `read()` that already holds its own reference waits for a
line the server may never send. So the reader waits on a poll with a deadline and checks its
stop flag between waits, and `Stop()` sets that flag first, closes the child's stdin second
(the end of its input, which most servers leave on), then joins.

The child itself is given a signal and a bounded wait, then killed. A server is not obliged to
leave on its own, and a stop that hangs is worse than a process killed earlier than it liked.

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
the dependency order and every module's entry points are globals. `web/state.js` runs
first and owns the one mutable object, `PU`: the socket, the streaming flags, the
assistant message being written, the chain length, and the composer's elements. Every
other module reads and writes `PU` rather than keeping a copy, so a fact has exactly one
home and no module has to be told about another's.

| File | Owns |
| :--- | :--- |
| `web/state.js` | The `PU` object, the frame-coalesced render queue, the connection dot's state |
| `web/block-thinking.js`, `web/block-tool.js` | The three block custom elements and their own drawing |
| `web/block-node.js` | Picking the element a block type becomes, and the folding a block shares |
| `web/message.js` | The frame around a message: role label, container, edit affordance |
| `web/transcript.js` | The assistant message in flight, block patching, the chunk handlers |
| `web/layout.js` | Which way the panel opens, and remembering that choice across the column and drawer layouts |
| `web/approval.js` | The card parked above the composer while a tool call waits for an answer |
| `web/panel.js` | What the sidebar reads rather than chooses: backend label, context bar, tool list |
| `web/notice.js` | The strip above the composer |
| `web/history.js` | Folding stored messages back into the bubbles a reader saw |
| `web/ws.js` | The socket, its reconnect policy, the frame dispatch table |
| `web/session.js` | The REST calls: session, history, thinking, agents, rewind |
| `web/composer.js` | Turning a press into a run, and a second press into a cancel |
| `web/app.js` | The element lookups and the start-up order, and nothing else |

A transcript block and its element are separate things: the block is plain data (`type`,
`content`, `collapsed`, …) and the element is a custom element that draws itself from it
on connect. `sync(block)` is the only way in, so a patch and a first paint take the same
path, and the coalesced re-render in `transcript.js` only has to decide *which* blocks
bear re-reading rather than how to draw them.

The block list is a timeline, in the order the turn happened: a reasoning chunk, a tool
call and a further reasoning chunk are three blocks, not one merged by kind. A new chunk
joins the block it follows only when that block is the same kind, which is the rule both
`transcript.js` and `history.js` apply, so a replayed history and a live stream are the
same shape.

A block that folds says so by setting `collapsible`, and `block-node.js` builds and
re-reads the header, the body and the arrow for it; an element that folds is not named
anywhere else.

Whether the view follows the reply is a question asked of the transcript element at the
moment of use, never a stored answer. Content grows under the reader without any scroll
event, so a flag set only by scrolling goes stale the instant a bubble is appended: it
then reads "the reader has scrolled away" while the reader is still at the bottom, and
the reply stops following for the rest of the turn. `isAtBottom()` therefore measures the
element each time it is asked, and a writer reads it *before* it appends, because the
append is the very thing that moves the content being measured. A reader who scrolls up
is left where they are; returning to the bottom resumes following with no other gesture.

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

## Peer Messaging

Two `pu serve` instances can talk to each other, each driven by its own agent and
its own model. A declares B in its peer list and vice versa; either side can then
start a conversation with the other and receive an answer.

```
     ┌─────────────────────────┐                ┌─────────────────────────┐
     │  pu serve (A, :8911)    │                │  pu serve (B, :8912)    │
     │                         │  POST          │                         │
     │  agent calls            │  /api/peer/    │  SessionRunner          │
     │  send_to_peer ──────────┼──message──────►│  ::SubmitFromPeer       │
     │                         │                │         │               │
     │  (returns at once)      │                │         ▼               │
     │                         │                │  a normal turn, run     │
     │                         │                │  on the worker thread   │
     │                         │                │         │               │
     │                         │                │         ▼               │
     │                         │                │  send_to_peer back      │
     │  a new turn             │◄───────────────┼─────────────────────────┘
     │  is queued              │                │
     └─────────────────────────┘                └─────────────────────────┘
```

### Delivery is asynchronous

`send_to_peer` does not wait for the peer to think. It posts the message, checks
that the peer accepted it, and returns; the asking agent is told plainly that an
answer will arrive later. The peer's reply is injected as a **new turn**, so a
slow or busy peer cannot hold up the sender, and the two agents can work at
different speeds.

This is why the tool reports "*has the message. It will answer in a later turn*"
rather than a reply: if it returned the peer's words, the call would block for as
long as the peer's model takes, and a long answer would look like a hang.

### A peer turn may answer only the agent that asked

The gate that stops an endless A→B→A→B chain is that a turn which arrived from a
peer may reach **only its sender**. `send_to_peer` therefore compares the
requested peer against `ToolContext::peer_origin`:

- a turn started by a human has no origin, so every configured peer is reachable;
- a turn started by a peer may reply to that peer, and to nobody else.

Replying to the sender must stay allowed, or a peer could never answer and the
conversation could never close. Relaying onward is refused with a reason the
model can read, so it learns the rule and answers directly instead of retrying.
The chain therefore ends by construction at the second hop: A can ask B, B can
answer A, and that answer cannot be forwarded to a third agent.

Hop counting is deliberately **not** the guard. It is carried for logging only,
because a counter can only guess at intent and a low ceiling would cut off a
legitimate long exchange between the same two agents. Structure, not a number,
decides when the conversation stops.

### Trust boundary

The peer endpoint is the one surface that lets a remote conversation drive a
local model, so it is authenticated while the rest of the HTTP API is not.

| Control | Effect |
| --- | --- |
| `Authorization: Bearer <token>` | Required; compared in constant time |
| Peer allowlist (`peers[]`) | An undeclared `from` is refused before any turn starts |
| Loopback binding | The listener is not exposed beyond the host by default |

`PU_PEER_TOKEN` overrides the token in `peers.json`, so the secret can stay out of
the file. A server with no `peers.json` does not mount the endpoint at all — the
route reports that peer messaging is not configured, and the tool is never
registered.

**How a peer's turn reaches a page.** A turn that arrived from a peer runs with no
page attached, since the page may not be open. Its frames are not lost to that:
`SessionRunner` keeps a record per turn, bounded to the last `kTurnsKept` (8), and
hands the turn in flight together with the last finished one to a page that
attaches afterwards. A page already attached when the frames are produced receives
them as they happen, so a reply that arrives while the page is open needs no
reload. A turn older than the window is still readable from `/api/history`, which
carries the result rather than the stream.

The turn keeps who asked: `UserPayload::from_peer` holds the peer's name, which
the page draws where it would otherwise say "You", and which the model never
reads — it is not part of the request. Such a turn cannot be rewound through
`/api/rewind`; it belongs to the conversation it arrived in.

Configuration lives in `.pu/peers.json`:

```json
{
  "self": { "name": "A", "host": "127.0.0.1", "port": 8911 },
  "token": "shared-secret",
  "peers": [{ "name": "B", "url": "http://127.0.0.1:8912" }]
}
```

Both sides must agree on the token and on each other's names. A peer entry
without a name and a url is skipped with a warning rather than trusted.

**Known gap:** `/api/*` and `/ws` remain unauthenticated, so any local process can
already drive a turn. That is a separate concern from peer messaging and is not
addressed here; it is tracked on its own.

---

## Persistence

```
<workspace>/.pu/session.json   # Single session state
./.pu/logs/                    # Log output, moved by PU_HOME
```

`<workspace>` is the directory `pu` was started in. Logs are the only thing
`PU_HOME` moves; the environment table in [configuration.md](configuration.md#environment-variables)
owns both paths.

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
├── app/                     ├── app/                  # main, CLI parsing, serve
├── config/                  ├── config/               # agents.json, the backend model
├── core/                    ├── core/                 # logging, platform, HTTP client
├── llm/                     ├── gateways/             # every gateway and its table
├── mcp/                     ├── llm/                  # protocols, providers, streaming parser
├── runtime/                 ├── mcp/                  # transports, JSON-RPC client
│   ├── command_router.hpp   ├── runtime/              # runtime, executor, command router
│   ├── executor.hpp         ├── session/              # Session, Conversation, message graph
│   └── runtime.hpp          └── tools/                # Toolbox, tools
├── session/                 # message graph, Session, Conversation
└── tools/
```

A header lives in its module's directory; `include/pu/runtime/` holds the
orchestration layer (`Runtime`, `Executor`, `CommandRouter`). A header whose
module has no directory of its own stays beside the sources that include it.

The build is one static library, `pu_core` (everything but `main.cpp`), and the
`pu` executable links it.

`web/` is the page the server mounts, served from the file list in
`kPageFiles` (`src/app/serve_http_routes.cpp`) — adding a file there means adding
it in both places, and a target outside the list is a `404` rather than a read of
the directory. Its module split is described under
[Browser modules](#browser-modules).

### Gates, protocols and providers

Three facts about a vendor that look alike are three different things: a **protocol** is
*how to talk*, a **gateway** is *which product*, and a connection is *where*. `config/gateway.hpp`
holds the middle one: `GatewayId` names the gateways a build carries, `Gateway` is
`{id, name, default_host, make_provider, make_listing}`, and `Gateways()` / `FindGateway()`
/ `GatewayForId()` answer from one explicit table (`kGateways`) in that file. Every gateway
lives in `src/gateways/gateways.cpp` with its neighbours: a gateway is mostly a table row plus
the mapping from the backend config to its protocol's config, and those mappings are only
readable as a set — side by side, a field one gateway fills and another drops is visible, and
in one file per gateway it is not. `MakeOpenAIProvider` and `MakeOllamaProvider` share
`FillProtocolConfig`, which is why the Ollama mapping cannot drift from the OpenAI one.

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
state — see [design/gateway-protocol-layout.md](design/gateway-protocol-layout.md).

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

- **New gateway**: Add a `Make<Name>Provider`, a `Make<Name>Listing`, a `<Name>Gateway()` and a table row in `src/gateways/gateways.cpp`, stating the name, the default host, how to build the gateway's provider, and how to build its catalogue read; then add the pointer to `kGateways`. A gateway that speaks an existing protocol reuses its provider and inherits `StreamingProvider`; one that does not supplies its own. Removing a gateway is the same in reverse - the four fragments and its line.
- **New tool**: Inherit `pu::Tool`, implement methods, register in `Runtime::RegisterBuiltinTools()`.
- **New command**: Add handler in `CommandRouter`, route, update help.
- **External tool (no C++)**: Add an `mcp_servers` entry to `agents.json` — tools are discovered automatically when the agent becomes active.
- **New peer (no C++)**: Add an entry to `.pu/peers.json` naming the peer and its URL, and agree on the shared token; the other side needs no restart.

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
- **Nothing enforces a token budget, by design.** `ChatResult::usage` carries what the provider counted, and the executor logs it at `debug`, but no limit is compared against it, so a conversation still grows until the provider refuses it and the refusal reaches the user as an HTTP error.
- **A `max_tokens` cap that lands inside a tool call ends the turn with an error.** The reply stops mid-argument, so the call never completed and its text is not a usable argument; the provider raises rather than keeping the fragment. A `finish_reason` other than `length` leaves a non-JSON argument as it stands.
- **A cancelled run keeps no partial reply, by design.** The transport aborts the stream and the executor ends the turn with neither a reply nor an error, so nothing is appended: the session holds the user message and no answer, and a follow-up "continue" restarts the answer rather than resuming it.
- **The store is only persisted after a completed interaction and on shutdown.** A crash loses everything since the last save, and the store is held in memory in between.
- **A stopped MCP server is killed rather than negotiated with.** The child gets `SIGTERM`, two seconds of polls, then `SIGKILL`; a server that needs longer to flush on the way out does not get it.
- **The transcript fold is checked by running the page's own JavaScript only where `node` exists.** Without it the same case file runs against a second implementation in Python, which catches a case that has drifted but not a page that no longer runs.

