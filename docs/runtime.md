# Runtime, lifecycle, and cancellation

How one `Runtime` instance is built, kept alive, stopped, and shared between the
command line and `pu serve`. This is the half of the system a *maintainer* has to hold in
their head: the lifecycle, the cancellation contract, the tool loop and the stream.

The split is a reading aid, not a boundary: every claim below is still about the
same binary, and a change that moves work between `Runtime` and `Executor` belongs
in both files' vocabularies.


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
(`RunServe` in `src/serve/serve.cpp`), on the host and port that the command line, the
environment, or the workspace's own `serve` block names:

1. `Runtime::Initialize()` loads `agents.json` and restores the session, then the
   server mounts the static `web/` UI and registers the HTTP routes.
2. The port it bound is read back from the acceptor rather than taken from the request,
   so a server started on port `0` still knows where to find itself, and it writes that
   address into `$HOME/.pu/serve.json` along with the directory it serves and its pid.
   That file is how the page names the machine's servers; a record whose process is gone
   is dropped on the next read, and the entry is deleted when the server stops. The read
   and the write happen under a lock file beside it, because two servers started together
   would otherwise each read the list before the other had written and the second write
   would erase the first.
3. An Asio `io_context` drives the `tcp::acceptor` on its own thread, with
   `reuse_address` set before the bind so a restart can take a port a just-closed
   server left connections in `TIME_WAIT` on.
4. Each accepted connection is dispatched on a strand of its own. A plain HTTP request
   is read, dispatched to the static/REST routes, and answered on that strand, and the
   connection ends with the response. Both the read and the write are asynchronous: a
   socket parked in a blocking read would hold the one thread the whole server runs on,
   so a browser's idle preconnect would stall every other page until it gave up. The
   request line and headers get a 30s deadline, and a connection that sends none is
   dropped. A WebSocket upgrade on `/ws` is handed to a `WebSocketConnection`, whose I/O
   is asynchronous from there on.
5. A second upgrade while a page is attached is answered with a `busy` frame and closed:
   one session, one page. Claiming the session happens when the upgrade is accepted, not
   when the first run arrives, so the refusal reaches the caller before its socket goes.
   The page names itself with `?client=` on the upgrade, and the claim is compared by that
   name: the same tab reconnecting takes the session back and the socket it left behind is
   closed from its own strand, while another tab is refused. Without a name, every
   connection is a page of its own.
6. A run is queued to a single `SessionRunner` worker, which runs `ProcessInput` and
   posts each frame back through the connection. The runner holds only the token that
   steers the turn; the worker reads it by value, so a `cancel` on the strand reaches a
   turn in flight without waiting for it. The message schema, the REST route table, and
   the status each refusal carries are in [web-api.md](web-api.md).
7. On Ctrl+C the acceptor is closed, the `io_context` is stopped — which aborts every
   pending read and write, so no connection thread is left blocked — the runner is
   stopped, the server removes its line from the registry, and `Runtime::Shutdown()`
   persists the session.

The registry is a file rather than a running daemon because nothing else would exist to
own the list: each server is independent, and a process that has stopped is exactly the
one that cannot delete its own line. `LiveServeRecords()` therefore trusts the pid and
not the file, which also means a pid the operating system has since handed to something
else reads as live; that costs a stale row in a list the page shows, and nothing reads a
record to decide where to send work.

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

`DispatchHttpRequest` (`src/serve/serve_http_routes.cpp`) does all the work a handler
should not have to: it finds the row for the request, asks `Admit` whether this caller may
read it, parses the body once for routes that take one, takes the `io_mutex` for the
duration of the call, wraps whatever the handler returned in the envelope that row names,
and turns a thrown exception into the status that names whose fault it is
(`RequestRefused` → `400`, `NotFound` → `404`, `Error` → `500`).

A handler therefore only computes and returns, against a `Call` rather than a `Runtime`:

```cpp
boost::json::value HandleApiRewind(Call& call, const boost::json::value& body);
```

`Call` carries the runtime, plus the facts that belong to the connection rather than to the
session: the address the request arrived from, whether that address is this machine's, and
the port this server answers on. A handler that had to reach for the socket to learn any of
those would be a second place for the audience rule to live.

One envelope, declared per row. `Route::key` names the member a payload is filed under
(`messages`, `agents`, `servers`), and an empty key means the handler's own object is the
answer; either way `success` is added by the dispatcher. The wire format is read off the
route table instead of being reassembled by hand in every handler, and a new listing cannot
invent a fourth shape.

`Route::audience` is the other column that matters, and it has no default: adding a route
means answering who may read it. That is what makes a route answering this machine's
directories impossible to add as one that answers the network by omission.

The status the exception maps to is the one that names whose fault it is: `RequestRefused`
for a request the caller can fix, `NotFound` for a name or route that does not exist, and
`Error` for an operation that failed on our side. The audience refusals are separate and are
answered before the handler runs: `403` for an address the route does not answer. Every route
this server answers is answered under `io_mutex`, since every one of them reads or changes
the session.

### Static page serving

Page resolution and the served-file allowlist are in [web-ui.md](web-ui.md).

### Single-session auto-persistence

`Runtime` maintains a single `std::shared_ptr<Session> current_session_`. On
`Initialize()` it loads `<workspace>/.pu/session.json` (if the file exists) via
`Session::Deserialize`. After every `ProcessInput()` call and on `Shutdown()`, the
session is serialized back to the same file via `Session::Serialize`. There is no
manual save/load/list/export; persistence is fully automatic and scoped to the
data directory.

The two paths are `<workspace>/.pu/session.json` and `<workspace>/.pu/logs/`, where
`<workspace>` is the directory the run is about: the one `-C` names, and the directory
`pu` was started in when it names none. Logs are the only thing `PU_HOME` moves, and their
directory is per process so two processes in one workspace never rotate the same file; the
environment table in [configuration.md](configuration.md#environment-variables) owns both
paths.

The file carries `schema_version` beside `conversation` and `session_spec`, and a file
stamped with a version this build does not accept is refused rather than read
best-effort: `pu` reports the reason and starts a fresh conversation, and the next save
replaces the file. The session file contains no system prompt — the prompt is
configuration and is read from `agents.json` on every start.

**What each turn cost is kept beside the conversation, not inside it.** `usage` is a
sibling of `conversation` in the file, keyed by the assistant node the turn produced:
prompt, completion and cached tokens, the credit the gateway priced it at, how many
provider calls the turn took, and which model and gateway answered. A model never reads
it, `History()` never renders it, and a page draws it next to the reply rather than as
part of it — which is why it is not a field of `AssistantPayload`. It is kept because it
is the only record that an optimization changed anything: a prompt edited so the cache
stops hitting, or a model swapped for one that costs three times as much, shows up here
and nowhere else. The key is absent from every file written before it existed, and absent
is read as "nothing was recorded" rather than as a version this build cannot load, so the
schema version did not move. A turn that has left the chain — a rewound turn, `/clear` —
takes its entry with it on the next save.

`session_spec` names the agent and carries the session's own thinking level when
`/thinking` set one. Backend fields are read from `agents.json` on each start, so
editing the configuration takes effect without touching the session. `/backend <type>
<model>` adopts the model named by the file, so the backend a session reports and the
agent it names are always the same answer. It takes no key: a key typed at the prompt is
written into the scrollback, so an endpoint that needs one is named in `agents.json`,
where it can come from the environment. A restart resumes the agent the session names; a
name the file no longer carries is reported and the file's own agent is used, since a file
describes exactly one.

The conversation is a chain: `conversation.history` holds `nodes`, each with its id,
timestamp, its parent and one role payload, plus the `leaf` that marks the current
position. A payload's `content` is a single string, and reasoning is a list of blocks,
each with the provider's `text`, an optional `signature` and the provider's own `raw`
encoding when it round-trips (see [providers.md](providers.md)).

A node's id is its own, not a position: the earlier `id = size() + 1` scheme made a
rewind representable only by rewriting the file, and a branch not at all.

`/rewind` moves the `leaf` back and removes nothing, so the turns after it stay in the
file until something replaces them. The append that follows drops whatever the new leaf
cannot reach, so a replaced turn leaves nothing behind and the store ends up holding
exactly the chain the view shows.

A tool call has three states: `pending` while it has not been answered, `completed` once a
receipt answers it, and `interrupted` when the turn that requested it was cancelled before
the call ran. Cancelling seals the leaf: every call still `pending` becomes `interrupted`
and gains a receipt recording that it never ran. Those receipts are chain nodes like any
other, so the next request renders them to the model as `role: "tool"` messages. Only a
`pending` call counts as unfinished, so a sealed turn stops blocking `/rewind` and a switch
of agent while a call that may still be running goes on blocking both. Whether a receipt
records a result or an interruption, its presence is what keeps each tool call paired with
an answer, which is what the request path is rebuilt from.

Three vocabularies name these states, and they are not interchangeable. Storage and the
session file use `pending` / `completed` / `interrupted`
([`message.hpp`](../include/pu/session/message.hpp), [`graph.cpp`](../src/session/graph.cpp)).
`/api/history` renames `completed` to `done` for the Web UI, so a client sees `pending` /
`done` / `interrupted`. The transcript additionally shows `running`, which is live-only
state and is never written to a session.

### The request path

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
([`request.hpp`](../include/pu/session/request.hpp)), which is the view a provider
requires. That makes `ChatMessage` a compatibility view rather than a place to grow: a new
context feature belongs to `MessageNode` ([`message.hpp`](../include/pu/session/message.hpp)),
which owns what a turn is.

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

### Delegation

`ask_dir` is the one tool that runs a turn rather than a command, and the only place a
`Runtime` is built by another `Runtime`. `Runtime::Delegate()` (`src/runtime/runtime.cpp`)
constructs a second instance, sets its `workspace_root_` to the target directory, marks it
`ephemeral_` and `delegated_`, initializes it, and calls `ProcessInput` — the same path the
user's own turn takes, so a delegated turn has the same executor, the same tool loop and the
same receipts. Nothing about the child is a special case of the loop; it is a second loop.

**One hop, by construction.** The child's `delegated_` flag is what withholds the tool: at
`RegisterBuiltinTools()` the `ask_dir` tool is registered `if (has("ask_dir") && !delegated_)`,
so a delegated turn's toolbox has no way to delegate further. The rule is a fact about which
toolbox was built rather than a counter that a call could get wrong.

**The child owns its own rules.** A directory target must already carry `.pu/agents.json`;
nothing creates one. That file — not the parent's — decides the child's sandbox, approval
patterns and model, which is why a parent cannot widen or narrow its child's
reach, and why `Inside()` in the parent only ever guards the `ask` string itself.

**Stop and deadline cross the boundary.** These are not tickets the asking turn can hold, so
the `Delegation` struct carries them in two forms rather than one:

| Field | What it is |
| :---- | :--------- |
| `cancel_token` | the asking turn's own token, handed down rather than copied — one button stops both turns |
| `timeout` | a ceiling on the whole delegated turn; zero means the asking turn's own platform default |

`AskDirTool` supplies the ceiling (`kDelegatedTurnBudget`, ten minutes, generous because a
sub-agent reading a repository is doing the work it was asked for). A ceiling has no token to
cancel, and a process going down has none either, so `Runtime::StopRequested()` folds all
three — the turn's token, the deadline it was given, and `platform::ShutdownRequested()` —
into one question, and `Executor::SetStopCheck()` is where the executor asks it. It is asked
at two places, and both are needed: at the top of each round, before the model is called, and
before each tool call in the round. One check alone buys a reply the user already stopped, or
a tool call they will never read the result of. The round counter increments when a round
opens, so a turn stopped before asking anything still reports the round it abandoned.

**The token is injected per turn, not captured.** A `Toolbox` outlives a turn — `ProcessInput`
reuses the one `Initialize()` built — but the cancel token belongs to the turn, and the serve
loop makes a new one per request. So `ProcessInput` walks `Toolbox::Tools()` and hands the
current token to `AskDirTool::SetCancelToken()` before the loop starts; a token captured in
the constructor would be the previous turn's.

**What comes back.** `DelegationResult` carries the child's answer, its error, its usage, and
a `stopped` flag that separates a stop from a fault: a stopped child produces "was still
answering when the turn was stopped, so its answer was dropped", and one that outlived its
deadline produces a message naming the budget and suggesting a smaller question. Neither is
`has_error` on the asking turn's behalf — they are a result the parent model can read and act
on, which is what keeps a stop from being reported as a broken child.

### The tool loop

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
`include/pu/agent_tools/tool.hpp`: `success`, `blocked`, `output`, `error`, `exit_code`, and the
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

### Paging and the request view

`BuildRequestPath` reads the graph and writes the message list. Nothing it does is stored:
a rewind, a reload or a second reader sees the graph as it was, which is what lets four
tiers apply to the same transcript without any of them being a fact about it. The tiers are
applied per node, monotone — a decision may only move a message down, never back up:

| Tier | What survives | Enters when |
|---|---|---|
| live | the whole text | newest run for its source, or a page read deliberately after |
| receipt | verdict, `page`, `buffer`, `extent`, `superseded_by` | a newer run of the same source was read |
| tombstone | `success`, `blocked`, `error`, `refusal`, `stored`, `buffer.{id,source}`, `elided` — no cursor | it is a superseded receipt and more than the newest six receipts are |
| gone | nothing | it is one of everything but the newest ten tool-call pairs |
| arguments | every argument key, with one text replaced by `bytes`, `lines`, `omitted` | the call survived the previous tier and its serialised arguments exceed 16 KiB |

The last two are the ones that remove anything, and their order is load-bearing: the tier
that drops pairs runs first, so the tier that shrinks arguments spends its work only on the
calls that reach the next turn. Working on a pair that is about to be deleted would be work
done for a message that never leaves.

**Retiring pairs.** A `tool` message whose `tool_call_id` names no call is rejected by every
provider, and a call with no receipt leaves the model waiting for a result that never comes,
so this tier removes a call and its receipt together or not at all. Ten survive rather than
six because the tombstone tier fires on a superseded receipt older than the newest six; a
retirement that also kept six would delete exactly the receipts the tombstone was about to
shrink and leave it nothing to do.

**Shrinking arguments.** An argument body is the input half of the exchange whose output half
the tiers above already compress, and it is the one part of a surviving call that can be
large. A `write_file` of a thousand lines costs its content in tokens on every request for as
long as it is retained, while the confirmation it produced was reduced to a verdict long
before. The stand-in keeps every key and takes only the text of the one that dominates —
three quarters of the string bytes — so a `write_file` still says which file it wrote and a
`bash` call still says what it ran. Only the largest text goes, and only when it does not
leave the call ambiguous.

Two things this tier does *not* do. It does not touch `arguments` in the record: the
substitution happens in `ToolCallToJson`, which is what builds the provider's view, so the
session still holds the text the provider was told and a reader of the graph sees the real
call. And it does not store the body in the pager, although `pu_session` can reach it. A
pager evicts under the output budget, and a body evicted from it would take the command a
model needs to repeat — the argv axis, which every tier here is written not to touch.

The thresholds differ in kind on purpose. The two tiers that remove work are driven by a
count, because a count is what the transcript naturally has. The argument tier is driven by
bytes, because what it removes is a literal string re-sent verbatim and the unit of that harm
is the length of the string; it is not a claim about how much context the model has left.

---

### MCP servers

```
McpClient            handshake, ListTools, CallTool
    │
JsonRpcClient        JSON-RPC 2.0: request/response, a promise per id
    │
Transport            Start / Stop / WriteLine
    │
    ├── StdioTransport   a child process's stdio, one line of JSON per message
    └── HttpTransport    BeastHttpClient POST, line-delimited responses
```

Servers are configured per-agent via `mcp_servers`, and a list may hold any number: each is
started as its own client and its tools are registered under the `mcp.<server>.` prefix.
`McpClient::Connect()` picks the transport — a non-empty `url` selects `HttpTransport`,
otherwise a stdio child is spawned, on both POSIX (`fork`/`execvp`) and Windows
(`CreateProcess`). `Runtime::RebuildToolbox` starts the active agent's servers, performs the
handshake and lists tools.

**How MCP and `agent_tools` meet.** The two libraries are siblings with no edge between
them: `pu_agent_tools` states what a turn may call and `pu_mcp` states how a server is
reached, and neither has a reason to know the other exists. The join is
`runtime::McpTool` ([`mcp_tool.hpp`](../include/pu/runtime/mcp_tool.hpp)), a `Tool` whose
body is one `CallTool` — written as a member of either library, that library would link the
other for no reason but this file. `RegisterMcpTools()` wraps each `ToolDefinition` from
`ListTools()` in an `McpTool` and registers it in the same `Toolbox` as `bash` and
`write_file`, so a turn calls both through one interface and the model tells them apart only
by the name prefix. The one asymmetry worth knowing: an approval pattern names a builtin by
name, so an MCP tool is not gated by it — its reach is whatever the server declares.

### Stopping an MCP server

A stdio server is a child process with three pipes, and stopping it is where a client most
easily hangs itself. Closing the descriptor a reader is blocked on does **not** wake that
read: the close succeeds, and the `read()` that already holds its own reference waits for a
line the server may never send. So the reader waits on a poll with a deadline and checks its
stop flag between waits, and `Stop()` sets that flag first, closes the child's stdin second
(the end of its input, which most servers leave on), then joins.

The child itself is given a signal and a bounded wait, then killed. A server is not obliged to
leave on its own, and a stop that hangs is worse than a process killed earlier than it liked.

---

## Streaming

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

The page itself is plain scripts with no build step, so the load order in `web/index.html`
*is* the dependency order. The modules and the static page resolution are in
[web-ui.md](web-ui.md).

---

## Toolbox & MCP lifecycle

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

