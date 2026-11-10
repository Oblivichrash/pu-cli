]633;E;printf '%s\\n' '# Runtime, lifecycle, and cancellation' '' 'How one `Runtime` instance is built, kept alive, stopped, and shared between the' 'command line and `pu serve`. This was part of [ARCHITECTURE.md](ARCHITECTURE.md)\x3b' 'it lives here because it is the half of the system a *maintainer* has to hold in' 'their head, while the rest of that document is what a contributor reads once.' '' 'The split is a reading aid, not a boundary: every claim below is still about the' 'same binary, and a change that moves work between `Runtime` and `Executor` belongs' 'in both files'"'"' vocabularies.' '';9ac79c07-acb2-4715-897b-dddcabfce472]633;C# Runtime, lifecycle, and cancellation

How one `Runtime` instance is built, kept alive, stopped, and shared between the
command line and `pu serve`. This was part of [ARCHITECTURE.md](ARCHITECTURE.md);
it lives here because it is the half of the system a *maintainer* has to hold in
their head, while the rest of that document is what a contributor reads once.

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
(`RunServe` in `src/app/serve.cpp`), on the host and port that the command line, the
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

`DispatchHttpRequest` (`src/app/serve_http_routes.cpp`) does all the work a handler
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
patterns, model and tool budget, which is why a parent cannot widen or narrow its child's
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

